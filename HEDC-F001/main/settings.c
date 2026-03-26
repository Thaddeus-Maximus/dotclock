#include "settings.h"

#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "display.h"
#include "audio.h"

static const char *TAG = "settings";
#define NVS_NAMESPACE "dotclock"

static uint8_t brightness = 1;
static int volume = AUDIO_VOLUME_MAX;
static uint8_t alarm_hour = 7;
static uint8_t alarm_minute = 0;
static bool alarm_enabled = false;
static char alarm_file[64] = "alarm.mp3";
static char wifi_ssid[33] = "";
static char wifi_pass[64] = "";

static void save_u8(const char *key, uint8_t val)
{
	nvs_handle_t h;
	if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
		nvs_set_u8(h, key, val);
		nvs_commit(h);
		nvs_close(h);
	}
}

static uint8_t load_u8(const char *key, uint8_t def)
{
	nvs_handle_t h;
	uint8_t val = def;
	if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
		nvs_get_u8(h, key, &val);
		nvs_close(h);
	}
	return val;
}

static void save_str(const char *key, const char *val)
{
	nvs_handle_t h;
	if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
		nvs_set_str(h, key, val);
		nvs_commit(h);
		nvs_close(h);
	}
}

static void load_str(const char *key, char *buf, size_t len, const char *def)
{
	nvs_handle_t h;
	if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
		size_t rlen = len;
		if (nvs_get_str(h, key, buf, &rlen) != ESP_OK) {
			strncpy(buf, def, len - 1);
			buf[len - 1] = '\0';
		}
		nvs_close(h);
	} else {
		strncpy(buf, def, len - 1);
		buf[len - 1] = '\0';
	}
}

void settings_init(void)
{
	brightness = load_u8("brightness", 1);
	volume = load_u8("volume", AUDIO_VOLUME_MAX);
	alarm_hour = load_u8("alm_hour", 7);
	alarm_minute = load_u8("alm_min", 0);
	alarm_enabled = load_u8("alm_en", 0) != 0;
	load_str("alm_file", alarm_file, sizeof(alarm_file), "alarm.mp3");
	load_str("wifi_ssid", wifi_ssid, sizeof(wifi_ssid), "");
	load_str("wifi_pass", wifi_pass, sizeof(wifi_pass), "");

	display_set_brightness(brightness);
	audio_set_volume(volume);

	ESP_LOGI(TAG, "Settings loaded: bright=%d vol=%d alarm=%02d:%02d %s file=%s",
	         brightness, volume, alarm_hour, alarm_minute,
	         alarm_enabled ? "ON" : "OFF", alarm_file);
}

void settings_set_brightness(uint8_t level)
{
	if (level > DISPLAY_BRIGHTNESS_MAX) level = DISPLAY_BRIGHTNESS_MAX;
	brightness = level;
	display_set_brightness(level);
	save_u8("brightness", level);
}

uint8_t settings_get_brightness(void)
{
	return brightness;
}

void settings_set_volume(int vol)
{
	if (vol < 0) vol = 0;
	if (vol > AUDIO_VOLUME_MAX) vol = AUDIO_VOLUME_MAX;
	volume = vol;
	audio_set_volume(vol);
	save_u8("volume", (uint8_t)vol);
}

int settings_get_volume(void)
{
	return volume;
}

void settings_set_alarm_time(uint8_t hour, uint8_t minute)
{
	if (hour > 23) hour = 23;
	if (minute > 59) minute = 59;
	alarm_hour = hour;
	alarm_minute = minute;
	save_u8("alm_hour", hour);
	save_u8("alm_min", minute);
}

uint8_t settings_get_alarm_hour(void)
{
	return alarm_hour;
}

uint8_t settings_get_alarm_minute(void)
{
	return alarm_minute;
}

void settings_set_alarm_enabled(bool enabled)
{
	alarm_enabled = enabled;
	save_u8("alm_en", enabled ? 1 : 0);
}

bool settings_get_alarm_enabled(void)
{
	return alarm_enabled;
}

void settings_set_alarm_file(const char *filename)
{
	if (!filename) return;
	strncpy(alarm_file, filename, sizeof(alarm_file) - 1);
	alarm_file[sizeof(alarm_file) - 1] = '\0';
	save_str("alm_file", alarm_file);
}

const char *settings_get_alarm_file(void)
{
	return alarm_file;
}

void settings_set_wifi(const char *ssid, const char *pass)
{
	if (ssid) {
		strncpy(wifi_ssid, ssid, sizeof(wifi_ssid) - 1);
		wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
		save_str("wifi_ssid", wifi_ssid);
	}
	if (pass) {
		strncpy(wifi_pass, pass, sizeof(wifi_pass) - 1);
		wifi_pass[sizeof(wifi_pass) - 1] = '\0';
		save_str("wifi_pass", wifi_pass);
	}
}

void settings_get_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
	if (ssid && ssid_len > 0) {
		strncpy(ssid, wifi_ssid, ssid_len - 1);
		ssid[ssid_len - 1] = '\0';
	}
	if (pass && pass_len > 0) {
		strncpy(pass, wifi_pass, pass_len - 1);
		pass[pass_len - 1] = '\0';
	}
}
