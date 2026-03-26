#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_PIN 25  // DAC1
#define AUDIO_VOLUME_MAX 16

void audio_init(void);
void audio_play(const char *filepath);  // e.g. "/storage/alarm.mp3"
void audio_stop(void);
bool audio_is_playing(void);
void audio_set_volume(int vol);    // 0 (mute) .. AUDIO_VOLUME_MAX (full)
int  audio_get_volume(void);
