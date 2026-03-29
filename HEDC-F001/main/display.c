#include "display.h"

#include <string.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"

// MAX7219 registers
#define REG_NOOP        0x00
#define REG_DIGIT0      0x01
#define REG_DECODEMODE  0x09
#define REG_INTENSITY   0x0A
#define REG_SCANLIMIT   0x0B
#define REG_SHUTDOWN    0x0C
#define REG_DISPLAYTEST 0x0F

static spi_device_handle_t spi_dev;
static uint8_t framebuf[DISPLAY_NUM_MODULES * 8];  // 8 rows per module
static portMUX_TYPE spi_mux = portMUX_INITIALIZER_UNLOCKED;

static void draw_glyph_3x6(int x, int y, int glyph_idx);

// Software PWM for sub-minimum brightness (levels 1-8)
static void pwm_timer_cb(void *arg);
static esp_timer_handle_t pwm_timer;
static volatile uint8_t pwm_duty = 0;   // 0-8: out of 8 ticks ON
static volatile uint8_t pwm_phase = 0;  // current tick counter
static uint8_t current_brightness = 1;
static bool flipped = false;

/*
// 3x5 font — commented out, using 3x6 font exclusively
static const uint8_t font_3x5[][3] = {
	[0]  = { 0x1F, 0x11, 0x1F },  // 0
	[1]  = { 0x00, 0x1F, 0x00 },  // 1
	[2]  = { 0x1D, 0x15, 0x17 },  // 2
	[3]  = { 0x15, 0x15, 0x1F },  // 3
	[4]  = { 0x07, 0x04, 0x1F },  // 4
	[5]  = { 0x17, 0x15, 0x1D },  // 5
	[6]  = { 0x1F, 0x15, 0x1D },  // 6
	[7]  = { 0x01, 0x01, 0x1F },  // 7
	[8]  = { 0x1F, 0x15, 0x1F },  // 8
	[9]  = { 0x17, 0x15, 0x1F },  // 9
	[10] = { 0x00, 0x04, 0x04 },  // minus
	[11] = { 0x00, 0x00, 0x00 },  // space
	[12] = { 0x1C, 0x0A, 0x1C },  // a
	[13] = { 0x1F, 0x14, 0x08 },  // b
	[14] = { 0x0E, 0x0A, 0x0A },  // c
	[15] = { 0x08, 0x14, 0x1F },  // d
	[16] = { 0x0E, 0x0A, 0x06 },  // e
	[17] = { 0x1E, 0x05, 0x01 },  // f
	[18] = { 0x12, 0x15, 0x0F },  // g
	[19] = { 0x1F, 0x04, 0x18 },  // h
	[20] = { 0x00, 0x1D, 0x00 },  // i
	[21] = { 0x10, 0x10, 0x0D },  // j
	[22] = { 0x1F, 0x04, 0x1A },  // k
	[23] = { 0x00, 0x1F, 0x10 },  // l
	[24] = { 0x1E, 0x04, 0x1E },  // m
	[25] = { 0x1E, 0x02, 0x1C },  // n
	[26] = { 0x0C, 0x12, 0x0C },  // o
	[27] = { 0x1E, 0x0A, 0x04 },  // p
	[28] = { 0x04, 0x0A, 0x1E },  // q
	[29] = { 0x1C, 0x02, 0x02 },  // r
	[30] = { 0x14, 0x1E, 0x0A },  // s
	[31] = { 0x02, 0x1F, 0x02 },  // t
	[32] = { 0x1E, 0x10, 0x1E },  // u
	[33] = { 0x0E, 0x10, 0x0E },  // v
	[34] = { 0x1E, 0x08, 0x1E },  // w
	[35] = { 0x1A, 0x04, 0x1A },  // x
	[36] = { 0x12, 0x14, 0x0E },  // y
	[37] = { 0x1A, 0x12, 0x16 },  // z
};
*/

// 3x6 font (LSB = top row), same indexing as font_3x5:
// 0-9 = digits, 10 = minus, 11 = space, 12-37 = a-z
static const uint8_t font_3x6[][3] = {
	[0]  = { 0x3F, 0x21, 0x3F },  // 0
	[1]  = { 0x21, 0x3F, 0x20 },  // 1
	[2]  = { 0x39, 0xA9, 0xAF },  // 2
	[3]  = { 0x25, 0x25, 0x3F },  // 3
	[4]  = { 0x07, 0x04, 0x3F },  // 4
	[5]  = { 0x27, 0x25, 0x3D },  // 5
	[6]  = { 0x3F, 0x25, 0x3D },  // 6
	[7]  = { 0x01, 0x01, 0x3F },  // 7
	[8]  = { 0x3F, 0x25, 0x3F },  // 8
	[9]  = { 0x27, 0x25, 0x3F },  // 9
	[10] = { 0x04, 0x04, 0x04 },  // minus
	[11] = { 0x00, 0x00, 0x00 },  // space
	[12] = { 0x3E, 0x05, 0x3E },  // A  .#. #.# ### #.# #.# #.#
	[13] = { 0x3F, 0x25, 0x1A },  // B  ##. #.# ##. #.# #.# ##.
	[14] = { 0x1E, 0x21, 0x21 },  // C  .## #.. #.. #.. #.. .##
	[15] = { 0x3F, 0x21, 0x1E },  // D  ##. #.# #.# #.# #.# ##.
	[16] = { 0x3F, 0x25, 0x21 },  // E  ### #.. ##. #.. #.. ###
	[17] = { 0x3F, 0x05, 0x01 },  // F  ### #.. ##. #.. #.. #..
	[18] = { 0x1E, 0x21, 0x3D },  // G  .## #.. #.# #.# #.# .##
	[19] = { 0x3F, 0x04, 0x3F },  // H  #.# #.# ### #.# #.# #.#
	[20] = { 0x21, 0x3F, 0x21 },  // I  ### .#. .#. .#. .#. ###
	[21] = { 0x11, 0x21, 0x1F },  // J  ### ..# ..# ..# #.# .#.
	[22] = { 0x3F, 0x0A, 0x31 },  // K  #.# ##. #.. ##. #.# #.#
	[23] = { 0x3F, 0x20, 0x20 },  // L  #.. #.. #.. #.. #.. ###
	[24] = { 0x3F, 0x02, 0x3F },  // M  #.# ### #.# #.# #.# #.#
	[25] = { 0x3F, 0x06, 0x3F },  // N  #.# ### ### #.# #.# #.#
	[26] = { 0x1E, 0x21, 0x1E },  // O  .#. #.# #.# #.# #.# .#.
	[27] = { 0x3F, 0x05, 0x02 },  // P  ##. #.# ##. #.. #.. #..
	[28] = { 0x0E, 0x11, 0x3F },  // Q  .## #.# #.# #.# .## ..#
	[29] = { 0x3F, 0x05, 0x3A },  // R  ##. #.# ##. #.# #.# #.#
	[30] = { 0x22, 0x25, 0x19 },  // S  .## #.. .#. ..# ..# ##.
	[31] = { 0x01, 0x3F, 0x01 },  // T  ### .#. .#. .#. .#. .#.
	[32] = { 0x1F, 0x20, 0x1F },  // U  #.# #.# #.# #.# #.# .#.
	[33] = { 0x0F, 0x30, 0x0F },  // V  #.# #.# #.# #.# .#. .#.
	[34] = { 0x3F, 0x18, 0x3F },  // W  #.# #.# #.# ### ### #.#
	[35] = { 0x33, 0x0C, 0x33 },  // X  #.# #.# .#. .#. #.# #.#
	[36] = { 0x03, 0x3C, 0x03 },  // Y  #.# #.# .#. .#. .#. .#.
	[37] = { 0x39, 0x25, 0x23 },  // Z  ### ..# .#. #.. #.. ###
	[38] = { 0x00, 0x20, 0x00 },  // .  (dot)
};

static int char_to_glyph(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'z') return 12 + (c - 'a');
	if (c >= 'A' && c <= 'Z') return 12 + (c - 'A');
	if (c == '-') return 10;
	if (c == '.') return 38;
	return 11;  // space / unknown
}

static void send_all(uint8_t reg, const uint8_t *values)
{
	// Send one 16-bit command per module, all in one transaction.
	// Modules are daisy-chained: first word goes to last module.
	uint8_t buf[DISPLAY_NUM_MODULES * 2];
	for (int i = 0; i < DISPLAY_NUM_MODULES; i++) {
		buf[i * 2]     = reg;
		buf[i * 2 + 1] = values[i];
	}

	spi_transaction_t txn = {
		.length = DISPLAY_NUM_MODULES * 16,
		.tx_buffer = buf,
	};
	portENTER_CRITICAL(&spi_mux);
	spi_device_polling_transmit(spi_dev, &txn);
	portEXIT_CRITICAL(&spi_mux);
}

static void send_all_same(uint8_t reg, uint8_t value)
{
	uint8_t vals[DISPLAY_NUM_MODULES];
	memset(vals, value, sizeof(vals));
	send_all(reg, vals);
}

void display_init(void)
{
	spi_bus_config_t bus_cfg = {
		.mosi_io_num = DISPLAY_PIN_DIN,
		.miso_io_num = -1,
		.sclk_io_num = DISPLAY_PIN_CLK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = DISPLAY_NUM_MODULES * 2,
	};
	spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_DISABLED);

	spi_device_interface_config_t dev_cfg = {
		.clock_speed_hz = 1 * 1000 * 1000,  // 1 MHz
		.mode = 0,
		.spics_io_num = DISPLAY_PIN_CS,
		.queue_size = 1,
	};
	spi_bus_add_device(SPI3_HOST, &dev_cfg, &spi_dev);

	send_all_same(REG_DISPLAYTEST, 0x00);
	send_all_same(REG_SCANLIMIT, 0x07);
	send_all_same(REG_DECODEMODE, 0x00);
	send_all_same(REG_SHUTDOWN, 0x01);
	send_all_same(REG_INTENSITY, 0x00);

	// Software PWM timer for sub-minimum dimming (~500Hz, 8 phases)
	esp_timer_create_args_t timer_cfg = {
		.callback = pwm_timer_cb,
		.name = "disp_pwm",
	};
	esp_timer_create(&timer_cfg, &pwm_timer);

	display_clear();
	display_update();
	display_set_brightness(1);
}

static void pwm_timer_cb(void *arg)
{
	pwm_phase = (pwm_phase + 1) % 8;
	send_all_same(REG_SHUTDOWN, (pwm_phase < pwm_duty) ? 0x01 : 0x00);
}

uint8_t display_get_brightness(void)
{
	return current_brightness;
}

void display_set_brightness(uint8_t level)
{
	if (level > DISPLAY_BRIGHTNESS_MAX) level = DISPLAY_BRIGHTNESS_MAX;
	current_brightness = level;

	if (level == 0) {
		// Off: set duty to 0 BEFORE stopping timer to prevent race
		// where an in-flight callback re-enables the display
		pwm_duty = 0;
		esp_timer_stop(pwm_timer);
		send_all_same(REG_SHUTDOWN, 0x00);
	} else if (level <= 8) {
		// Software PWM: duty 1-8 out of 8, at hardware intensity 0
		pwm_duty = 0;  // prevent glitch during reconfiguration
		esp_timer_stop(pwm_timer);
		send_all_same(REG_INTENSITY, 0x00);
		pwm_phase = 0;
		pwm_duty = level;
		send_all_same(REG_SHUTDOWN, 0x01);
		esp_timer_start_periodic(pwm_timer, 250);  // 250us per phase = ~500Hz
	} else {
		// Hardware brightness: levels 9-24 map to intensity 0-15
		pwm_duty = 0;
		esp_timer_stop(pwm_timer);
		send_all_same(REG_SHUTDOWN, 0x01);
		send_all_same(REG_INTENSITY, level - 9);
	}
}

void display_clear(void)
{
	memset(framebuf, 0, sizeof(framebuf));
}

void display_invert(void)
{
	for (int i = 0; i < (int)sizeof(framebuf); i++)
		framebuf[i] ^= 0xFF;
}

void display_set_flip(bool flip)
{
	flipped = flip;
}

// Reverse bits in a byte (mirror columns within a module)
static uint8_t reverse_byte(uint8_t b)
{
	b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
	b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
	b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
	return b;
}

void display_update(void)
{
	// MAX7219 digits 0-7 correspond to rows 0-7.
	// Each module gets its own column of 8 bits per row.
	// FC-16 layout: module 0 is leftmost, columns go left to right.
	for (int row = 0; row < 8; row++) {
		uint8_t vals[DISPLAY_NUM_MODULES];
		if (flipped) {
			// 180-degree rotation: reverse row order, reverse module
			// order, and mirror bits within each module
			int src_row = 7 - row;
			for (int m = 0; m < DISPLAY_NUM_MODULES; m++) {
				vals[m] = reverse_byte(
					framebuf[(DISPLAY_NUM_MODULES - 1 - m) * 8 + src_row]);
			}
		} else {
			for (int m = 0; m < DISPLAY_NUM_MODULES; m++) {
				vals[m] = framebuf[m * 8 + row];
			}
		}
		send_all(REG_DIGIT0 + row, vals);
	}
}

// Set a pixel in the framebuffer. x=0 is leftmost, y=0 is top.
static void fb_set_pixel(int x, int y, bool on)
{
	if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT)
		return;

	int module = x / 8;
	int col_in_module = x % 8;

	// FC-16 modules: bit 0 of the row register = rightmost column of module
	uint8_t mask = 1 << (7 - col_in_module);
	if (on)
		framebuf[module * 8 + y] |= mask;
	else
		framebuf[module * 8 + y] &= ~mask;
}

/*
// Draw a 3x5 glyph at position (x, y=offset from top)
static void draw_glyph(int x, int y, int glyph_idx)
{
	if (glyph_idx < 0 || glyph_idx > 37) return;
	const uint8_t *glyph = font_3x5[glyph_idx];
	for (int col = 0; col < 3; col++) {
		uint8_t bits = glyph[col];
		for (int row = 0; row < 5; row++) {
			fb_set_pixel(x + col, y + row, bits & (1 << row));
		}
	}
}
*/

void display_number(int32_t num)
{
	display_clear();

	bool negative = false;
	if (num < 0) {
		negative = true;
		num = -num;
	}

	// Convert to digit array
	int digits[10];
	int ndigits = 0;
	if (num == 0) {
		digits[ndigits++] = 0;
	} else {
		while (num > 0 && ndigits < 10) {
			digits[ndigits++] = num % 10;
			num /= 10;
		}
	}

	// Each glyph is 3 wide + 1 gap = 4 pixels. Right-align on display.
	int total_width = ndigits * 4 - 1;  // no trailing gap
	if (negative) total_width += 4;      // minus sign + gap

	int x = DISPLAY_WIDTH - total_width;
	int y = 1;  // vertically center 6px font in 8px height

	if (negative) {
		draw_glyph_3x6(x, y, 10);  // minus
		x += 4;
	}

	for (int i = ndigits - 1; i >= 0; i--) {
		draw_glyph_3x6(x, y, digits[i]);
		x += 4;
	}
}

void display_text(const char *str)
{
	display_clear();
	int x = 1;
	int y = 1;
	for (int i = 0; str[i] && x < DISPLAY_WIDTH; i++) {
		int g = char_to_glyph(str[i]);
		draw_glyph_3x6(x, y, g);
		x += 4;
	}
}

// Draw a 3x6 glyph at position (x, y)
static void draw_glyph_3x6(int x, int y, int glyph_idx)
{
	if (glyph_idx < 0 || glyph_idx > 38) return;
	const uint8_t *glyph = font_3x6[glyph_idx];
	for (int col = 0; col < 3; col++) {
		uint8_t bits = glyph[col];
		for (int row = 0; row < 6; row++) {
			fb_set_pixel(x + col, y + row, bits & (1 << row));
		}
	}
}

void display_time(uint8_t hour, uint8_t minute, bool colon)
{
	display_clear();
	int y = 1;  // center 6px font in 8px height

	// Layout: [4][3][2][3][3][2][3][3][2][3][4] = 32
	draw_glyph_3x6(4,  y, hour / 10);    // col 4-6
	draw_glyph_3x6(9,  y, hour % 10);    // col 9-11

	// Colon: 2 cols wide, double-thick (0b01100110)
	if (colon) {
		//fb_set_pixel(15, y + 0, true);
		//fb_set_pixel(16, y + 0, true);
		fb_set_pixel(15, y + 1, true);
		fb_set_pixel(16, y + 1, true);
		fb_set_pixel(15, y + 4, true);
		fb_set_pixel(16, y + 4, true);
		//fb_set_pixel(15, y + 5, true);
		//fb_set_pixel(16, y + 5, true);
	}

	draw_glyph_3x6(20, y, minute / 10);  // col 20-22
	draw_glyph_3x6(25, y, minute % 10);  // col 25-27
}

void display_dashes(void)
{
	display_clear();
	int y = 1;

	// Same layout as display_time but with dashes (glyph 10) instead of digits
	draw_glyph_3x6(4,  y, 10);
	draw_glyph_3x6(9,  y, 10);

	// Colon
	fb_set_pixel(15, y + 1, true);
	fb_set_pixel(16, y + 1, true);
	fb_set_pixel(15, y + 4, true);
	fb_set_pixel(16, y + 4, true);

	draw_glyph_3x6(20, y, 10);
	draw_glyph_3x6(25, y, 10);
}

// 8x8 mode icons, one byte per row (top to bottom)
static const uint8_t icons[][8] = {
	[DISPLAY_ICON_ALARM]      = { 0x00,0x04,0x3C,0x7E,0x7E,0x3C,0x04,0x00 },
	[DISPLAY_ICON_SET_TIME]   = { 0x40,0x5E,0x42,0x42,0x20,0x10,0x0F,0x00 },
	[DISPLAY_ICON_VOLUME]     = { 0x18,0x3C,0x7E,0x00,0x5A,0x42,0x3C,0x00 },
	[DISPLAY_ICON_BRIGHTNESS] = { 0x00, 0x3C, 0x7E, 0x7E, 0x42, 0x42, 0x3C, 0x00 },
	[DISPLAY_ICON_NETWORK]    = { 0x00, 0x0F, 0x00, 0x1F, 0x00, 0x3F, 0x00, 0x7F },
};

// Draw an 8x8 icon on panel 0 (cols 0-7)
// Icon data is 8 columns, each byte has 8 rows (MSB = top)
static void draw_icon(display_icon_t icon)
{
	const uint8_t *data = icons[icon];
	for (int col = 0; col < 8; col++) {
		uint8_t bits = data[col];
		for (int row = 0; row < 8; row++) {
			fb_set_pixel(col, row, bits & (1 << (7 - row)));
		}
	}
}

void display_icon_time(display_icon_t icon, uint8_t hour, uint8_t minute)
{
	display_clear();
	draw_icon(icon);

	// HHMM in 3x6 font, centered in panels 1-3 (cols 8-31)
	// [4 pad][3][1][3][2][3][1][3][4 pad] = 24
	int y = 1;
	draw_glyph_3x6(12, y, hour / 10);
	draw_glyph_3x6(16, y, hour % 10);
	draw_glyph_3x6(21, y, minute / 10);
	draw_glyph_3x6(25, y, minute % 10);
}

void display_icon_number(display_icon_t icon, int num)
{
	display_clear();
	draw_icon(icon);

	// Number in 3x6 font, centered in panels 1-3 (cols 8-31)
	int y = 1;
	if (num < 10) {
		// Single digit: center in 24 cols
		draw_glyph_3x6(19, y, num);
	} else {
		// Two digits: center pair (3+1+3 = 7 wide) in 24 cols
		draw_glyph_3x6(17, y, num / 10);
		draw_glyph_3x6(21, y, num % 10);
	}
}

void display_icon_text(display_icon_t icon, const char *str)
{
	display_clear();
	draw_icon(icon);

	// Text in 3x6 font, centered in panels 1-3 (cols 8-31)
	int y = 1;
	int len = 0;
	for (const char *p = str; *p; p++) len++;
	int total_w = len * 4 - 1;  // 3px glyph + 1px gap, no trailing gap
	int x = 8 + (24 - total_w) / 2;
	for (int i = 0; str[i]; i++) {
		draw_glyph_3x6(x, y, char_to_glyph(str[i]));
		x += 4;
	}
}

int display_text_width(const char *str)
{
	int len = 0;
	for (const char *p = str; *p; p++) len++;
	return len > 0 ? len * 4 - 1 : 0;
}

void display_text_scroll(const char *str, int pixel_offset)
{
	display_clear();
	int y = 1;
	int x = -pixel_offset;
	for (int i = 0; str[i]; i++) {
		if (x + 3 >= 0 && x < DISPLAY_WIDTH)
			draw_glyph_3x6(x, y, char_to_glyph(str[i]));
		x += 4;
		if (x >= DISPLAY_WIDTH) break;
	}
}
