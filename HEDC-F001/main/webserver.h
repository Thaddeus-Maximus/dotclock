#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t webserver_init(void);

// Call after settings_init to attempt STA connection if credentials stored
void webserver_try_sta_connect(void);

// Network status
bool webserver_is_sta_connected(void);
void webserver_get_sta_ip(char *buf, size_t len);

// Toggle SoftAP broadcasting (STA stays up either way)
void webserver_set_ap_enabled(bool enabled);
bool webserver_is_ap_enabled(void);

// AP SSID (for UI display)
const char *webserver_get_ap_ssid(void);
