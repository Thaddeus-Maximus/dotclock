#pragma once

#include <stdbool.h>
#include <stdint.h>

// 4x FC-16 modules (MAX7219), 32x8 pixels total
#define DISPLAY_NUM_MODULES 4
#define DISPLAY_WIDTH       32
#define DISPLAY_HEIGHT      8

#define DISPLAY_PIN_CS   17
#define DISPLAY_PIN_CLK  18
#define DISPLAY_PIN_DIN  23

// Brightness range: 0 = off, 1-8 = software PWM (sub-minimum),
// 9-24 = hardware levels 0-15. Total 25 steps.
#define DISPLAY_BRIGHTNESS_MAX 24

void display_init(void);
void display_set_brightness(uint8_t level);  // 0 to DISPLAY_BRIGHTNESS_MAX
uint8_t display_get_brightness(void);
void display_clear(void);
void display_invert(void);                   // invert entire framebuffer
void display_number(int32_t num);            // render a number, right-aligned
void display_text(const char *str);          // render text, left-aligned
void display_time(uint8_t hour, uint8_t minute, bool colon);  // render HH:MM (full width)
void display_dashes(void);                   // render --:-- (time not set)
void display_update(void);                   // flush framebuffer to hardware

// 8x8 mode icons for panel 0
typedef enum {
	DISPLAY_ICON_ALARM,
	DISPLAY_ICON_SET_TIME,
	DISPLAY_ICON_VOLUME,
	DISPLAY_ICON_BRIGHTNESS,
} display_icon_t;

void display_icon_time(display_icon_t icon, uint8_t hour, uint8_t minute);  // icon + HHMM
void display_icon_number(display_icon_t icon, int num);                      // icon + number
void display_icon_text(display_icon_t icon, const char *str);                // icon + text
