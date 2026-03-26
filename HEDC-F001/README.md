# HEDC-F001 Firmware

ESP-IDF v5.3.1 firmware for the dotclock alarm clock.

## Building

```
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

Flash size must be set to **16MB** in menuconfig (`Serial flasher config > Flash size`).

## Partition Layout

Defined in `partitions.csv`:

| Partition | Type | Size | Purpose |
|-----------|------|------|---------|
| nvs | data | 24KB | Settings (brightness, volume, alarm, WiFi) |
| otadata | data | 8KB | OTA state |
| ota_0 | app | 2MB | Application slot 0 |
| ota_1 | app | 2MB | Application slot 1 |
| storage | data | ~12MB | LittleFS filesystem for MP3 files |

## Source Files

| File | Description |
|------|-------------|
| `main.c` | App entry point, encoder UI state machine (5 modes), idle timeout |
| `display.c` | MAX7219 driver, framebuffer, 3x6 font, 8x8 mode icons, software PWM brightness |
| `display.h` | Display API, pin definitions, brightness constants, icon enum |
| `encoder.c` | Quadrature decoder via GPIO ISR, velocity detection, button debounce |
| `encoder.h` | Encoder pin definitions, state struct |
| `audio.c` | MP3 decode (libhelix-mp3) to 8-bit DAC via `dac_continuous` driver |
| `audio.h` | Audio API, volume constants |
| `settings.c` | NVS-backed persistent settings (load/save/getters/setters) |
| `settings.h` | Settings API |
| `alarm.c` | Alarm check timer (1s), looping playback, time management (epoch, timezone) |
| `alarm.h` | Alarm API, time helpers (`time_is_set`, `time_set_epoch`, `time_set_tz`) |
| `webserver.c` | HTTP server, 11 URI handlers, WiFi AP+STA, SNTP |
| `webserver.h` | Webserver API |
| `webpage.html` | Web UI source (compiled to `webpage.h` by `webpage_compile.py`) |
| `simple_dns_server.c` | Captive portal DNS responder |

## Dependencies

Defined in `idf_component.yml`:

- `chmorgan/esp-libhelix-mp3` ^1.0.3 -- MP3 decoding
- `joltwallet/littlefs` ^1.20.3 -- LittleFS filesystem

## Display

### Hardware

4x FC-16 modules (MAX7219), daisy-chained via SPI3 at 1MHz.

| Signal | GPIO |
|--------|------|
| CS | 17 |
| CLK | 18 |
| DIN | 23 |

### Fonts

Two fonts are defined (3x5 is commented out, 3x6 is active):

**3x6 font** (`font_3x6`): 3 columns, 6 rows per glyph, LSB = top row. Indices:
- 0-9: digits (user-customized shapes)
- 10: minus/dash
- 11: space
- 12-37: A-Z

### Rendering Functions

| Function | Description |
|----------|-------------|
| `display_time(h, m, colon)` | Full-width HH:MM with double-thick colon, y-offset 1 |
| `display_dashes()` | Full-width --:-- (same layout as time, for "not set" state) |
| `display_text(str)` | Left-aligned text |
| `display_number(num)` | Right-aligned number |
| `display_icon_time(icon, h, m)` | 8x8 icon on panel 0 + HHMM on panels 1-3 |
| `display_icon_number(icon, num)` | 8x8 icon on panel 0 + number centered on panels 1-3 |
| `display_icon_text(icon, str)` | 8x8 icon on panel 0 + text centered on panels 1-3 |
| `display_invert()` | XOR entire framebuffer (used for alarm flash) |

### Brightness

25 levels (0-24). `DISPLAY_BRIGHTNESS_MAX` = 24.

- Level 0: display off
- Levels 1-8: software PWM at hardware intensity 0 (~500Hz, 8 duty phases)
- Levels 9-24: hardware intensity registers 0-15

PWM race condition is handled by setting `pwm_duty = 0` before `esp_timer_stop()`.

### Mode Icons

8x8 pixel icons stored as column bytes (MSB = top), one per UI mode:

| Icon | Enum | Usage |
|------|------|-------|
| Bell | `DISPLAY_ICON_ALARM` | Alarm time setting |
| Clock | `DISPLAY_ICON_SET_TIME` | System time setting |
| Speaker | `DISPLAY_ICON_VOLUME` | Volume adjustment |
| Bulb | `DISPLAY_ICON_BRIGHTNESS` | Brightness adjustment |

## Encoder

PEC09 rotary encoder with quadrature decoding in ISR. `DRAM_ATTR` is required on the lookup table (ISR runs with flash cache disabled during NVS/LittleFS writes).

- 2 raw edges per detent, divided by 2 in `encoder_read()`
- Velocity detection via ring buffer of edge timestamps
- Fast threshold: 100 detents/sec
- Button debounce: 20ms in ISR

## Audio

- MP3 decode via libhelix-mp3 in a dedicated FreeRTOS task (priority 10)
- Output: `dac_continuous` driver on GPIO25 (DAC1), 8-bit mono
- DMA: 8 descriptors x 2048 bytes, input buffer 4096 bytes
- GPIO25 driven low when not playing to reduce idle speaker static
- Volume: 16 levels, applied as bit-shift attenuation on PCM samples

## Alarm

- 1-second `esp_timer` callback checks if current time matches alarm time
- When triggered, plays the selected MP3 file in a loop (re-triggers on playback end)
- Dismissed by encoder button press or knob turn
- Display flashes inverted at ~1.4Hz (350ms per phase) while ringing
- Alarm defaults to OFF; enabled by scrolling up from OFF in the alarm menu
- Scrolling below 00:00 sets alarm to OFF; scrolling above 23:59 wraps to 00:00

## Settings (NVS)

NVS namespace: `dotclock`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| brightness | u8 | 1 | Display brightness (1-24) |
| volume | u8 | 8 | Audio volume (0-16) |
| alm_hour | u8 | 0 | Alarm hour |
| alm_min | u8 | 0 | Alarm minute |
| alm_en | u8 | 0 | Alarm enabled |
| alm_file | string | "" | Alarm sound filename |
| wifi_ssid | string | "" | STA WiFi SSID |
| wifi_pass | string | "" | STA WiFi password |

## Web Server

WiFi SoftAP is always active: SSID `dotclock`, password `dotclock1`, IP `192.168.4.1`. Optional STA mode connects to a saved WiFi network (AP+STA concurrent).

### Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Serve web UI (compiled from `webpage.html`) |
| GET | `/settings` | JSON: all settings + current time + WiFi status |
| POST | `/settings` | Update settings (form-encoded) |
| POST | `/time` | Set epoch + timezone offset (from browser) |
| POST | `/wifi` | Save WiFi credentials and connect |
| GET | `/files` | JSON array of files on storage |
| POST | `/upload` | Upload file (multipart/form-data) |
| POST | `/delete` | Delete file (`?file=name`) |
| POST | `/rename` | Rename file (`?from=old&to=new`) |
| POST | `/alarm/test` | Play alarm sound immediately |
| POST | `/alarm/stop` | Stop alarm playback |

### Time Sync

The browser sends `Math.floor(Date.now()/1000)` as epoch and `new Date().getTimezoneOffset()` as tz offset. The ESP32 sets the system clock and configures a POSIX TZ environment variable. When STA WiFi is connected, SNTP is also enabled.
