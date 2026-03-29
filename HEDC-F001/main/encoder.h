#pragma once

#include <stdint.h>
#include <stdbool.h>

#define ENC_PIN_A   26
#define ENC_PIN_B   27
#define ENC_PIN_SW  14

typedef struct {
	int32_t position;       // cumulative detent count
	uint8_t speed;          // 0=slow, 1=medium, 2=fast
	bool fast;              // true = speed >= 2 (coarse adjust mode)
	bool button;            // true = pressed
	bool button_changed;    // true = state changed since last read
} encoder_state_t;

void encoder_init(void);
void encoder_set_invert(bool invert);
encoder_state_t encoder_read(void);
