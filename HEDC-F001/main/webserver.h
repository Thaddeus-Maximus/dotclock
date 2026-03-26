#pragma once

#include "esp_err.h"

esp_err_t webserver_init(void);

// Call after settings_init to attempt STA connection if credentials stored
void webserver_try_sta_connect(void);
