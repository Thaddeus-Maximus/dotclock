#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Initialize settings from NVS, apply to hardware
void settings_init(void);

// Brightness (0-24)
void settings_set_brightness(uint8_t level);
uint8_t settings_get_brightness(void);

// Volume (0-16)
void settings_set_volume(int vol);
int settings_get_volume(void);

// Alarm
void settings_set_alarm_time(uint8_t hour, uint8_t minute);
uint8_t settings_get_alarm_hour(void);
uint8_t settings_get_alarm_minute(void);

void settings_set_alarm_enabled(bool enabled);
bool settings_get_alarm_enabled(void);

void settings_set_alarm_file(const char *filename);
const char *settings_get_alarm_file(void);

// WiFi STA credentials
void settings_set_wifi(const char *ssid, const char *pass);
void settings_get_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
