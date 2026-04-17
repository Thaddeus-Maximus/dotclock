#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "encoder.h"
#include "display.h"
#include "audio.h"
#include "settings.h"
#include "alarm.h"
#include "webserver.h"

static const char *TAG = "main";

extern bool time_is_set(void);
extern void time_set_epoch(int64_t epoch);

typedef enum {
	MODE_TIME,
	MODE_ALARM,
	MODE_VOLUME,
	MODE_BRIGHTNESS,
	MODE_SET_TIME,
	MODE_NETWORK,
	MODE_COUNT
} ui_mode_t;

#define IDLE_TIMEOUT  1500  // 15 seconds at 10ms ticks

// Default epoch for initial time set (2025-01-01 12:00:00 UTC)
#define DEFAULT_EPOCH 1735732800LL

static void init_storage(void)
{
	esp_vfs_littlefs_conf_t cfg = {
		.base_path = "/storage",
		.partition_label = "storage",
		.format_if_mount_failed = true,
		.dont_mount = false,
	};
	esp_err_t ret = esp_vfs_littlefs_register(&cfg);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(ret));
		return;
	}

	size_t total = 0, used = 0;
	esp_littlefs_info("storage", &total, &used);
	ESP_LOGI(TAG, "Storage: %d KB total, %d KB used", total / 1024, used / 1024);
}

// Adjust alarm time by delta minutes
// Below 00:00 → OFF; above 23:59 → wraps to 00:00
// From OFF, scrolling up → 00:00 (enabled)
static void adjust_alarm_time(int delta)
{
	if (!settings_get_alarm_enabled()) {
		if (delta > 0) {
			settings_set_alarm_enabled(true);
			settings_set_alarm_time(0, 0);
			alarm_reschedule();
		}
		return;
	}

	int total = settings_get_alarm_hour() * 60 + settings_get_alarm_minute() + delta;

	if (total < 0) {
		// Scrolled below 00:00 → OFF
		settings_set_alarm_enabled(false);
		alarm_reschedule();
		return;
	}

	// Wrap at top (23:59 + 1 → 00:00, stays enabled)
	total = total % (24 * 60);

	settings_set_alarm_time(total / 60, total % 60);
	alarm_reschedule();
}

// Adjust the system clock by delta minutes
static void adjust_system_time(int delta)
{
	// Seed with a reasonable epoch if time was never set
	if (!time_is_set()) {
		time_set_epoch(DEFAULT_EPOCH);
	}

	struct timeval tv;
	gettimeofday(&tv, NULL);
	tv.tv_sec += delta * 60;
	if (tv.tv_sec < 0) tv.tv_sec = 0;
	settimeofday(&tv, NULL);
}

// Start/stop volume preview tone on mode transitions
static void on_mode_change(ui_mode_t prev, ui_mode_t next)
{
	if (next == MODE_VOLUME && prev != MODE_VOLUME) {
		// Entering volume mode: play alarm tone for preview
		char path[64];
		snprintf(path, sizeof(path), "/storage/%s", settings_get_alarm_file());
		audio_play(path);
	} else if (prev == MODE_VOLUME && next != MODE_VOLUME) {
		// Leaving volume mode: stop preview
		audio_stop();
	}

	if (next == MODE_NETWORK && prev != MODE_NETWORK) {
		webserver_set_ap_enabled(true);
	} else if (prev == MODE_NETWORK && next != MODE_NETWORK) {
		webserver_set_ap_enabled(false);
	}
}

// Network mode scroll state
static int net_scroll_offset = 0;
static int net_scroll_timer = 0;
#define NET_SCROLL_RATE  6   // pixels per tick (60ms per pixel at 10ms loop)
#define NET_SCROLL_PAUSE 40  // ticks to pause at start/end (400ms)

// Show the current value for the active mode
static void show_mode_value(ui_mode_t mode)
{
	switch (mode) {
	case MODE_TIME:
		if (time_is_set()) {
			time_t now;
			time(&now);
			struct tm tm;
			localtime_r(&now, &tm);
			display_time(tm.tm_hour, tm.tm_min, true);
		} else {
			display_dashes();
		}
		break;
	case MODE_VOLUME:
		display_icon_number(DISPLAY_ICON_VOLUME, settings_get_volume());
		break;
	case MODE_BRIGHTNESS:
		display_icon_number(DISPLAY_ICON_BRIGHTNESS, settings_get_brightness());
		break;
	case MODE_ALARM:
		if (settings_get_alarm_enabled()) {
			display_icon_time(DISPLAY_ICON_ALARM,
			                  settings_get_alarm_hour(),
			                  settings_get_alarm_minute());
		} else {
			display_icon_text(DISPLAY_ICON_ALARM, "off");
		}
		break;
	case MODE_SET_TIME: {
		time_t now;
		time(&now);
		struct tm tm;
		localtime_r(&now, &tm);
		display_icon_time(DISPLAY_ICON_SET_TIME, tm.tm_hour, tm.tm_min);
		break;
	}
	case MODE_NETWORK: {
		char info[32];
		webserver_get_sta_ip(info, sizeof(info));
		int text_w = display_text_width(info);
		if (text_w <= DISPLAY_WIDTH) {
			display_text(info);
			net_scroll_offset = 0;
			net_scroll_timer = 0;
		} else {
			int max_scroll = text_w - DISPLAY_WIDTH;
			net_scroll_timer++;
			if (net_scroll_timer >= NET_SCROLL_RATE) {
				net_scroll_timer = 0;
				net_scroll_offset++;
			}
			// Total cycle: pause + scroll + pause + reset
			int total = NET_SCROLL_PAUSE + max_scroll + NET_SCROLL_PAUSE;
			int phase = net_scroll_offset % total;
			int offset;
			if (phase < NET_SCROLL_PAUSE)
				offset = 0;
			else if (phase < NET_SCROLL_PAUSE + max_scroll)
				offset = phase - NET_SCROLL_PAUSE;
			else
				offset = max_scroll;
			display_text_scroll(info, offset);
		}
		break;
	}
	default:
		break;
	}
}

void app_main(void)
{
	encoder_init();
	display_init();
	init_storage();
	audio_init();
	webserver_init();
	settings_init();
	alarm_init();
	webserver_try_sta_connect();

	display_text("ready");
	display_update();
	ESP_LOGI(TAG, "WiFi AP: dotclock / dotclock1");

	ui_mode_t mode = MODE_TIME;
	int32_t prev_pos = 0;
	int idle_count = 0;
	int flash_count = 0;
	bool flash_inverted = false;

	while (1) {
		encoder_state_t enc = encoder_read();
		int32_t delta = enc.position - prev_pos;
		prev_pos = enc.position;

		// --- Button press: dismiss alarm or cycle mode ---
		if (enc.button_changed && enc.button) {
			if (alarm_is_ringing()) {
				alarm_dismiss();
				ui_mode_t prev = mode;
				mode = MODE_TIME;
				on_mode_change(prev, mode);
				idle_count = 0;
			} else {
				ui_mode_t prev = mode;
				mode = (mode + 1) % MODE_COUNT;
				on_mode_change(prev, mode);
				idle_count = 0;
				if (mode == MODE_NETWORK) {
					net_scroll_offset = 0;
					net_scroll_timer = 0;
				}
			}
			show_mode_value(mode);
		}

		// --- Knob turn: action depends on mode ---
		if (delta != 0) {
			idle_count = 0;

			switch (mode) {
			case MODE_TIME:
				if (alarm_is_ringing()) {
					alarm_dismiss();
				} else {
					int bri = settings_get_brightness() + delta;
					if (bri < 1) bri = 1;
					if (bri > DISPLAY_BRIGHTNESS_MAX) bri = DISPLAY_BRIGHTNESS_MAX;
					settings_set_brightness(bri);
				}
				break;

			case MODE_VOLUME: {
				int vol = settings_get_volume() + delta;
				settings_set_volume(vol);
				// Restart preview if it finished
				if (!audio_is_playing()) {
					char path[64];
					snprintf(path, sizeof(path), "/storage/%s", settings_get_alarm_file());
					audio_play(path);
				}
				break;
			}

			case MODE_BRIGHTNESS: {
				int bri = settings_get_brightness() + delta;
				if (bri < 1) bri = 1;
				if (bri > DISPLAY_BRIGHTNESS_MAX) bri = DISPLAY_BRIGHTNESS_MAX;
				settings_set_brightness(bri);
				break;
			}

			case MODE_ALARM: {
				static const int step_table[] = {1, 5, 15};
				adjust_alarm_time(delta * step_table[enc.speed]);
				break;
			}

			case MODE_SET_TIME: {
				static const int step_table[] = {1, 5, 15};
				adjust_system_time(delta * step_table[enc.speed]);
				break;
			}

			default:
				break;
			}

			show_mode_value(mode);
		}

		// --- Idle timeout: return to time mode (suspended in network mode) ---
		idle_count++;
		if (mode != MODE_TIME && mode != MODE_NETWORK && idle_count > IDLE_TIMEOUT) {
			on_mode_change(mode, MODE_TIME);
			mode = MODE_TIME;
		}

		// --- Periodic display refresh (clock ticks, etc.) ---
		if (delta == 0 && !(enc.button_changed && enc.button)) {
			show_mode_value(mode);
		}

		// --- Alarm flash: invert display at ~1.4Hz (350ms per phase) ---
		if (alarm_is_ringing()) {
			flash_count++;
			if (flash_count >= 35) {  // 350ms at 10ms ticks
				flash_count = 0;
				flash_inverted = !flash_inverted;
			}
			if (flash_inverted) {
				display_invert();
			}
		} else {
			flash_count = 0;
			flash_inverted = false;
		}

		display_update();
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}
