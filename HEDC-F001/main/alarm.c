#include "alarm.h"

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "audio.h"
#include "settings.h"

static const char *TAG = "alarm";

static esp_timer_handle_t alarm_timer;
static volatile bool ringing = false;
static bool fired_today = false;
static int last_check_day = -1;

bool time_is_set(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec > 1700000000;  // after ~2023
}

void time_set_tz(int offset_minutes)
{
	// offset_minutes is from JS getTimezoneOffset():
	// positive = west of UTC, negative = east of UTC
	// POSIX TZ uses same sign convention: "UTC+5" = 5h west = UTC-5 in ISO
	int hours = offset_minutes / 60;
	int mins = abs(offset_minutes) % 60;
	char tz[32];
	if (mins != 0) {
		snprintf(tz, sizeof(tz), "UTC%+d:%02d", hours, mins);
	} else {
		snprintf(tz, sizeof(tz), "UTC%+d", hours);
	}
	setenv("TZ", tz, 1);
	tzset();
	ESP_LOGI(TAG, "Timezone set: %s", tz);
}

void time_set_epoch(int64_t epoch)
{
	struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
	settimeofday(&tv, NULL);
	ESP_LOGI(TAG, "Time set to %lld", (long long)epoch);
	fired_today = false;
	last_check_day = -1;
}

int64_t time_get_epoch(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec;
}

static void alarm_check_cb(void *arg)
{
	// Replay alarm sound if it finished but wasn't dismissed
	if (ringing && !audio_is_playing()) {
		char path[80];
		snprintf(path, sizeof(path), "/storage/%s", settings_get_alarm_file());
		audio_play(path);
		return;
	}

	if (!settings_get_alarm_enabled() || !time_is_set() || ringing)
		return;

	time_t now;
	time(&now);
	struct tm tm;
	localtime_r(&now, &tm);

	// Reset fired flag on new day
	if (tm.tm_yday != last_check_day) {
		fired_today = false;
		last_check_day = tm.tm_yday;
	}

	if (fired_today)
		return;

	if (tm.tm_hour == settings_get_alarm_hour() &&
	    tm.tm_min == settings_get_alarm_minute()) {
		fired_today = true;
		ringing = true;

		char path[80];
		snprintf(path, sizeof(path), "/storage/%s", settings_get_alarm_file());
		ESP_LOGI(TAG, "ALARM! Playing %s", path);
		audio_play(path);
	}
}

void alarm_init(void)
{
	esp_timer_create_args_t cfg = {
		.callback = alarm_check_cb,
		.name = "alarm",
	};
	esp_timer_create(&cfg, &alarm_timer);
	esp_timer_start_periodic(alarm_timer, 1000000);  // 1 second
	ESP_LOGI(TAG, "Alarm timer started");
}

bool alarm_is_ringing(void)
{
	return ringing;
}

void alarm_dismiss(void)
{
	if (ringing) {
		ringing = false;
		audio_stop();
		ESP_LOGI(TAG, "Alarm dismissed");
	}
}

void alarm_reschedule(void)
{
	fired_today = false;
	last_check_day = -1;
}
