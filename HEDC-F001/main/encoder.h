#pragma once

#include <stdint.h>
#include <stdbool.h>

#define ENC_PIN_A   26
#define ENC_PIN_B   27
#define ENC_PIN_SW  14

typedef struct {
	int32_t position;       // cumulative detent count
	bool fast;              // true = turning fast (coarse adjust mode)
	bool button;            // true = pressed
	bool button_changed;    // true = state changed since last read
} encoder_state_t;

void encoder_init(void);
encoder_state_t encoder_read(void);
