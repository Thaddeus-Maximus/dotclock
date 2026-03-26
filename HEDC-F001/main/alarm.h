#pragma once

#include <stdbool.h>
#include <stdint.h>

// Start the 1-second alarm check timer
void alarm_init(void);

// Returns true if the alarm is currently ringing
bool alarm_is_ringing(void);

// Dismiss the current alarm
void alarm_dismiss(void);

// Call after changing alarm settings to recompute next trigger
void alarm_reschedule(void);
