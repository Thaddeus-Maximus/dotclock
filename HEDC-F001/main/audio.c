#include "audio.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/dac_continuous.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "mp3dec.h"

static const char *TAG = "audio";

#define AUDIO_DAC_GPIO  GPIO_NUM_25

// MP3 decode buffer sizes
#define MP3_INBUF_SIZE    4096
#define MP3_FRAME_SAMPLES 1152  // max samples per MP3 frame per channel

static dac_continuous_handle_t dac_handle = NULL;
static volatile bool playing = false;
static volatile bool stop_requested = false;
static TaskHandle_t play_task_handle = NULL;
static volatile int volume = AUDIO_VOLUME_MAX;  // 0..AUDIO_VOLUME_MAX

static void dac_init(int sample_rate)
{
	dac_continuous_config_t cfg = {
		.chan_mask = DAC_CHANNEL_MASK_CH0,  // GPIO25 = DAC channel 0
		.desc_num = 8,
		.buf_size = 2048,
		.freq_hz = sample_rate,
		.offset = 0,
		.clk_src = DAC_DIGI_CLK_SRC_APLL,
		.chan_mode = DAC_CHANNEL_MODE_SIMUL,
	};

	if (dac_handle) {
		dac_continuous_disable(dac_handle);
		dac_continuous_del_channels(dac_handle);
		dac_handle = NULL;
	}

	ESP_ERROR_CHECK(dac_continuous_new_channels(&cfg, &dac_handle));
	ESP_ERROR_CHECK(dac_continuous_enable(dac_handle));
}

static void dac_deinit(void)
{
	if (dac_handle) {
		dac_continuous_disable(dac_handle);
		dac_continuous_del_channels(dac_handle);
		dac_handle = NULL;
	}
	// Drive GPIO25 low to silence amp input (kills idle static)
	gpio_set_direction(AUDIO_DAC_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_level(AUDIO_DAC_GPIO, 0);
}

// Skip ID3v2 tag at start of file if present
static void skip_id3v2(FILE *f)
{
	uint8_t hdr[10];
	if (fread(hdr, 1, 10, f) != 10) {
		fseek(f, 0, SEEK_SET);
		return;
	}

	if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
		// ID3v2 size is stored as syncsafe integer in bytes 6-9
		uint32_t size = ((uint32_t)hdr[6] << 21) |
		                ((uint32_t)hdr[7] << 14) |
		                ((uint32_t)hdr[8] << 7) |
		                ((uint32_t)hdr[9]);
		fseek(f, 10 + size, SEEK_SET);
		ESP_LOGI(TAG, "Skipped ID3v2 tag (%lu bytes)", (unsigned long)(10 + size));
	} else {
		fseek(f, 0, SEEK_SET);
	}
}

static void play_task(void *arg)
{
	const char *filepath = (const char *)arg;

	FILE *f = fopen(filepath, "rb");
	if (!f) {
		ESP_LOGE(TAG, "Failed to open %s", filepath);
		playing = false;
		play_task_handle = NULL;
		vTaskDelete(NULL);
		return;
	}

	skip_id3v2(f);

	HMP3Decoder decoder = MP3InitDecoder();
	if (!decoder) {
		ESP_LOGE(TAG, "Failed to init MP3 decoder");
		fclose(f);
		playing = false;
		play_task_handle = NULL;
		vTaskDelete(NULL);
		return;
	}

	static uint8_t inbuf[MP3_INBUF_SIZE];
	static int16_t pcm[MP3_FRAME_SAMPLES * 2];  // stereo worst case
	static uint8_t dac_buf[MP3_FRAME_SAMPLES];   // 8-bit mono output
	int bytes_left = 0;
	int read_offset = 0;
	bool dac_started = false;

	while (!stop_requested) {
		// Refill input buffer
		if (bytes_left < MP3_INBUF_SIZE / 2) {
			memmove(inbuf, inbuf + read_offset, bytes_left);
			read_offset = 0;
			int bytes_read = fread(inbuf + bytes_left, 1,
			                       MP3_INBUF_SIZE - bytes_left, f);
			if (bytes_read == 0) {
				break;  // EOF
			}
			bytes_left += bytes_read;
		}

		// Find sync word
		uint8_t *read_ptr = inbuf + read_offset;
		int offset = MP3FindSyncWord(read_ptr, bytes_left);
		if (offset < 0) {
			break;  // no sync found, probably end of data
		}
		read_ptr += offset;
		bytes_left -= offset;
		read_offset += offset;

		// Decode one frame
		MP3FrameInfo info;
		int err = MP3Decode(decoder, &read_ptr, &bytes_left, pcm, 0);
		int consumed = (read_ptr - (inbuf + read_offset));
		read_offset += consumed;

		if (err) {
			if (err == ERR_MP3_INDATA_UNDERFLOW) {
				continue;  // need more data
			}
			ESP_LOGW(TAG, "MP3 decode error %d, skipping", err);
			continue;
		}

		MP3GetLastFrameInfo(decoder, &info);

		// Init DAC on first successful frame
		if (!dac_started) {
			ESP_LOGI(TAG, "MP3: %d Hz, %d ch, %d kbps",
			         info.samprate, info.nChans, info.bitrate / 1000);
			dac_init(info.samprate);
			dac_started = true;
		}

		// Convert 16-bit PCM to 8-bit unsigned for DAC, with volume
		int nsamples = info.outputSamps / info.nChans;
		int vol = volume;  // snapshot volatile
		for (int i = 0; i < nsamples; i++) {
			int32_t sample;
			if (info.nChans == 2) {
				sample = ((int32_t)pcm[i * 2] + pcm[i * 2 + 1]) / 2;
			} else {
				sample = pcm[i];
			}
			sample = sample * vol / AUDIO_VOLUME_MAX;
			// 16-bit signed -> 8-bit unsigned
			dac_buf[i] = (uint8_t)((sample + 32768) >> 8);
		}

		size_t bytes_written = 0;
		dac_continuous_write(dac_handle, dac_buf, nsamples, &bytes_written, -1);
	}

	MP3FreeDecoder(decoder);
	fclose(f);
	dac_deinit();

	ESP_LOGI(TAG, "Playback finished");
	playing = false;
	stop_requested = false;
	play_task_handle = NULL;
	vTaskDelete(NULL);
}

void audio_init(void)
{
	// Drive DAC pin low to silence amp until playback starts
	gpio_set_direction(AUDIO_DAC_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_level(AUDIO_DAC_GPIO, 0);
}

void audio_play(const char *filepath)
{
	if (playing) {
		audio_stop();
	}

	static char path_buf[128];
	strncpy(path_buf, filepath, sizeof(path_buf) - 1);
	path_buf[sizeof(path_buf) - 1] = '\0';

	playing = true;
	stop_requested = false;
	xTaskCreate(play_task, "mp3_play", 8192, path_buf, 10, &play_task_handle);
}

void audio_stop(void)
{
	if (playing) {
		stop_requested = true;
		while (playing) {
			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}
}

bool audio_is_playing(void)
{
	return playing;
}

void audio_set_volume(int vol)
{
	if (vol < 0) vol = 0;
	if (vol > AUDIO_VOLUME_MAX) vol = AUDIO_VOLUME_MAX;
	volume = vol;
}

int audio_get_volume(void)
{
	return volume;
}
