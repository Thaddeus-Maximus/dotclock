#include "encoder.h"

#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// IRAM-safe GPIO read (gpio_get_level lives in flash)
static inline uint32_t IRAM_ATTR gpio_read_iram(gpio_num_t pin)
{
	return gpio_ll_get_level(&GPIO, pin);
}

// Quadrature state table: [prev_AB][curr_AB] -> delta
// PEC09 with 15 detents gives 2 edges per detent per channel.
// We accumulate raw edges and convert to detents.
static volatile int32_t raw_count = 0;
static volatile uint8_t prev_ab = 0;

// Edge timestamps for velocity measurement
static volatile int64_t edge_time_prev = 0;
static volatile int64_t edge_time_last = 0;

// Speed tier thresholds (detents/sec)
#define SPEED_THRESH_MED   80   // not ~1 rev/s on 15-detent PEC09
#define SPEED_THRESH_FAST  300  // not ~2 rev/s
#define SPEED_HYST          5   // hysteresis band
#define STALENESS_US   150000   // 150ms: assume stopped

// Direction invert
static bool inverted = false;

// Button state
static volatile bool btn_raw = false;
static volatile bool btn_state = false;
static volatile bool btn_changed = false;
static volatile int64_t btn_last_change = 0;

#define BTN_DEBOUNCE_US 20000  // 20ms debounce

// Quadrature lookup: +1 = CW, -1 = CCW, 0 = invalid/no move
// DRAM_ATTR: must be in RAM, not flash, because ISR reads it while cache may be off
static const DRAM_ATTR int8_t quad_table[4][4] = {
	// to: 00  01  10  11
	{  0, -1, +1,  0 },  // from 00
	{ +1,  0,  0, -1 },  // from 01
	{ -1,  0,  0, +1 },  // from 10
	{  0, +1, -1,  0 },  // from 11
};

static void IRAM_ATTR encoder_isr(void *arg)
{
	uint8_t a = gpio_read_iram(ENC_PIN_A);
	uint8_t b = gpio_read_iram(ENC_PIN_B);
	uint8_t curr_ab = (a << 1) | b;

	int8_t delta = quad_table[prev_ab][curr_ab];
	prev_ab = curr_ab;

	if (delta != 0) {
		raw_count += delta;

		int64_t now = esp_timer_get_time();
		edge_time_prev = edge_time_last;
		edge_time_last = now;
	}
}

static void IRAM_ATTR button_isr(void *arg)
{
	int64_t now = esp_timer_get_time();
	if ((now - btn_last_change) < BTN_DEBOUNCE_US) {
		return;
	}

	bool level = !gpio_read_iram(ENC_PIN_SW);  // active LOW -> invert
	if (level != btn_raw) {
		btn_raw = level;
		btn_state = level;
		btn_changed = true;
		btn_last_change = now;
	}
}

void encoder_init(void)
{
	// Configure encoder pins as inputs (external pull-ups on board)
	gpio_config_t enc_cfg = {
		.pin_bit_mask = (1ULL << ENC_PIN_A) | (1ULL << ENC_PIN_B),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_ANYEDGE,
	};
	gpio_config(&enc_cfg);

	// Configure button pin
	gpio_config_t btn_cfg = {
		.pin_bit_mask = (1ULL << ENC_PIN_SW),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_ANYEDGE,
	};
	gpio_config(&btn_cfg);

	// Read initial state
	prev_ab = (gpio_get_level(ENC_PIN_A) << 1) | gpio_get_level(ENC_PIN_B);
	btn_state = !gpio_get_level(ENC_PIN_SW);
	btn_raw = btn_state;

	// Install ISR service and attach handlers
	gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
	gpio_isr_handler_add(ENC_PIN_A, encoder_isr, NULL);
	gpio_isr_handler_add(ENC_PIN_B, encoder_isr, NULL);
	gpio_isr_handler_add(ENC_PIN_SW, button_isr, NULL);
}

void encoder_set_invert(bool invert)
{
	inverted = invert;
}

static portMUX_TYPE enc_mux = portMUX_INITIALIZER_UNLOCKED;

// EMA + hysteresis state (only accessed from encoder_read, not ISR)
static int32_t ema_speed = 0;      // fixed-point x256
static uint8_t last_tier = 0;

encoder_state_t encoder_read(void)
{
	// Snapshot volatile state
	portENTER_CRITICAL(&enc_mux);
	int32_t raw = raw_count;
	int64_t t_prev = edge_time_prev;
	int64_t t_last = edge_time_last;
	bool pressed = btn_state;
	bool changed = btn_changed;
	btn_changed = false;
	portEXIT_CRITICAL(&enc_mux);

	int32_t position = inverted ? -(raw / 2) : (raw / 2);

	// Compute instantaneous speed from last two edges
	int64_t now = esp_timer_get_time();
	int64_t age = t_last > 0 ? now - t_last : STALENESS_US;
	int32_t inst_speed = 0;

	if (t_prev > 0 && t_last > t_prev && age < STALENESS_US) {
		int64_t interval = t_last - t_prev;
		// 2 edges per detent: detents/sec = 500000 / interval_us
		inst_speed = (int32_t)(500000 / interval);
	}

	// EMA smoothing (fixed-point x256, alpha ~= 0.25)
	if (age >= STALENESS_US) {
		// Stopped: reset immediately
		ema_speed = 0;
	} else {
		ema_speed = (ema_speed * 3 + inst_speed * 256) / 4;
	}
	int32_t smooth = ema_speed / 256;

	// Classify speed tier with hysteresis
	uint8_t tier = last_tier;
	if (tier == 0) {
		if (smooth >= SPEED_THRESH_MED + SPEED_HYST)
			tier = 1;
	} else if (tier == 1) {
		if (smooth < SPEED_THRESH_MED - SPEED_HYST)
			tier = 0;
		else if (smooth >= SPEED_THRESH_FAST + SPEED_HYST)
			tier = 2;
	} else {
		if (smooth < SPEED_THRESH_FAST - SPEED_HYST)
			tier = 1;
	}
	last_tier = tier;

	(void)tier;  // multispeed disabled — always slow

	return (encoder_state_t){
		.position = position,
		.speed = 0,
		.fast = false,
		.button = pressed,
		.button_changed = changed,
	};
}
