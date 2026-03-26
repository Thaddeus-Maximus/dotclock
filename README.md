# dotclock

An ESP32-based alarm clock with a dot matrix LED display, rotary encoder interface, MP3 audio playback, and a web configuration UI.

## Features

- 32x8 dot matrix display (4x FC-16 / MAX7219 modules) with custom 3x6 pixel font
- Rotary encoder for all on-device interaction (press to cycle modes, turn to adjust)
- MP3 alarm tones stored on-chip (LittleFS, ~12MB available)
- Web UI for settings, time sync, WiFi config, and file management
- 25-step brightness (8 software PWM + 16 hardware levels)
- Persistent settings via NVS

## User Interface

The encoder button cycles through modes. The knob adjusts the value for the current mode. After 15 seconds of inactivity, the display returns to time mode.

| Mode | Icon | Knob Action | Notes |
|------|------|-------------|-------|
| Time | (none, full-width HH:MM) | Dismisses alarm if ringing | Default mode |
| Volume | Speaker icon | 0-16 | Plays alarm tone as preview while adjusting |
| Brightness | Bulb icon | 1-24 | |
| Alarm | Bell icon | Adjusts alarm time by 1min (fast: 15min) | Scroll below 00:00 for OFF; defaults to OFF |
| Set Time | Clock icon | Adjusts system clock by 1min (fast: 15min) | |

When the alarm is ringing, the display flashes inverted at ~1.4Hz. Press button or turn knob to dismiss.

## Web Interface

WiFi AP: **dotclock** / password **dotclock1** / IP **192.168.4.1**

- Sync time from phone/browser (with timezone)
- Set alarm time, enable/disable, select alarm sound
- Adjust brightness and volume
- Connect to a WiFi network (AP+STA mode, enables SNTP)
- Upload, rename, and delete MP3 files

## Audio

- MP3 files, decoded via libhelix-mp3
- 8-bit DAC output on GPIO25 through a PAM8403 class-D amplifier
- GPIO25 driven low when idle to minimize speaker static

## Project Structure

- `HEDC-F001/` — Firmware (ESP-IDF v5.3.1 project)
- `HEDC-B002/` — Hardware (board schematic, see hardware reference below)

## TODO

- [x] Read encoder (debouncing, velocity detection)
- [x] Set display (writing, brightness control)
- [x] Custom 3x6 pixel font with 8x8 mode icons
- [x] MP3 audio playback
- [x] Alarm system with looping playback
- [x] Web interface (settings, file management, time sync)
- [x] Persistent settings (NVS)
- [x] WiFi AP + STA with SNTP
- [ ] Fine-tune font glyphs
- [ ] RC filter on DAC output for cleaner audio


# HEDC-B002 Hardware Reference

## Board Overview

The HEDC-B002 is an ESP32-based controller board with USB-C programming, SPI LED output (5V level-shifted), a rotary encoder with push button, an audio amplifier, and a 32.768kHz RTC crystal. The crystal, however, doesn't work right, it seems.

## Power

| Rail | Source | Regulator |
|------|--------|-----------|
| +5V | USB-C (J1) | Direct from VBUS |
| +3.3V | +5V rail | U4 AMS1117-3.3 (SOT-223) |

## ESP32 GPIO Map

### Used GPIOs

| GPIO | Direction | Net Label | Function | Notes |
|------|-----------|-----------|----------|-------|
| IO0 | - | UART_~BOOT | Boot mode select | Auto-reset circuit via Q1. Do not use at runtime. |
| IO1 | TX | UART_TXD | Serial TX to CH340C | USB serial. Directly to J3 header pin alternate. |
| IO3 | RX | UART_RXD | Serial RX from CH340C | USB serial. |
| IO14 | Input | ES1 | Rotary encoder push switch | 5.1kΩ pull-up to 3.3V. Active LOW. |
| IO17 | Output | CS | SPI chip select | 3.3V → 5V via U1 74HCT244 → J3 pin 4 (CS_5V) |
| IO18 | Output | CLK | SPI clock | 3.3V → 5V via U1 74HCT244 → J3 pin 5 (CLK_5V) |
| IO23 | Output | DIN | SPI data out | 3.3V → 5V via U1 74HCT244 → J3 pin 3 (DIN_5V) |
| IO25 | Output | INL | Audio output to amplifier | AC-coupled into PAM8403 left channel input |
| IO26 | Input | EA1 | Rotary encoder channel A | 5.1kΩ pull-up to 3.3V |
| IO27 | Input | EB1 | Rotary encoder channel B | 5.1kΩ pull-up to 3.3V |
| IO32 | - | CRYSTAL_1 | 32.768kHz crystal (XIN) | Y1 CM315D32768DZFT, 22pF load caps (C7, C8) |
| IO33 | - | CRYSTAL_2 | 32.768kHz crystal (XOUT) | Y1 CM315D32768DZFT, 22pF load caps (C7, C8) |
| EN | - | UART_EN | Chip enable | Auto-reset circuit via Q1 + RC timing cap |

### Unused GPIOs

| GPIO | Pin # | Notes |
|------|-------|-------|
| IO2 | 24 | Explicitly no-connect on schematic |
| IO4 | 26 | Available |
| IO5 | 29 | Available (outputs PWM at boot) |
| IO12 | 14 | Available (boot fail if pulled high) |
| IO13 | 16 | Available |
| IO15 | 23 | Available (outputs PWM at boot) |
| IO16 | 27 | Available |
| IO19 | 31 | Available |
| IO21 | 33 | Available |
| IO22 | 36 | Available |
| IO34 | 6 | Available (input only, no pull-up/down) |
| IO35 | 7 | Available (input only, no pull-up/down) |
| SENSOR_VP (IO36) | 4 | Available (input only) |
| SENSOR_VN (IO39) | 5 | Available (input only) |

## Components

### U3 — CH340C (USB-to-Serial)

SOIC-16 USB-UART bridge. Internal oscillator (no external crystal). Connected to USB-C connector J1.

| CH340C Pin | Net | Connects To |
|------------|-----|-------------|
| TXD | UART_TXD | ESP32 IO1 (RXD0) |
| RXD | UART_RXD | ESP32 IO3 (TXD0) |
| ~DTR | DTR | Q1 auto-reset circuit |
| ~RTS | RTS | Q1 auto-reset circuit |
| UD+/UD- | USB D+/D- | J1 USB-C connector |
| V3 | - | 100nF decoupling cap (C6) to GND |

### Q1 — MMDT3904-7-F (Auto-Reset)

Dual NPN transistor (SOT-363) implementing the standard ESP32 auto-programming circuit. Cross-coupled: RTS drives IO0 (BOOT), DTR drives EN, so a DTR/RTS sequence enters the bootloader automatically.

### U1 — 74HCT244 (Level Shifter)

TSSOP-20 octal buffer. Powered from +5V. Converts 3.3V ESP32 SPI signals to 5V for J3 output. Bank 1 active (OE tied to GND), bank 2 active but unused.

| Input (3.3V) | Output (5V) | Signal |
|--------------|-------------|--------|
| 1A0 ← CS (IO17) | 1Y0 → CS_5V | Chip select |
| 1A1 ← CLK (IO18) | 1Y1 → CLK_5V | Clock |
| 1A2 ← DIN (IO23) | 1Y2 → DIN_5V | Data |

### J3 — 5-Pin LED Output Connector (FC-16 footprint)

| Pin | Signal |
|-----|--------|
| 1 | +5V |
| 2 | GND |
| 3 | DIN_5V |
| 4 | CS_5V |
| 5 | CLK_5V |

This is the SPI output for driving an LED matrix or display panel.

### SW1 — PEC09-2320F-S0015 (Rotary Encoder)

Bourns horizontal-mount rotary encoder with push button. All signals pulled up to 3.3V with 5.1kΩ resistors.

| Function | Net | GPIO | Active State |
|----------|-----|------|-------------|
| Encoder A | EA1 | IO26 | LOW pulses |
| Encoder B | EB1 | IO27 | LOW pulses |
| Push switch | ES1 | IO14 | LOW when pressed |

### U2 — PAM8403DR (Audio Amplifier)

SOP-16 class-D stereo amplifier, powered from +5V. Left channel input (INL) driven from ESP32 IO25 through a coupling capacitor. Speaker output on SPK1 header (marked DNP — do not populate).

| Connection | Detail |
|------------|--------|
| Input | IO25 → coupling cap → INL (pin 7) |
| Output | LOUT+/LOUT- → SPK1 (2-pin header) |
| Power | +5V / GND |

### Y1 — CM315D32768DZFT (RTC Crystal)

32.768kHz crystal connected to IO32/IO33 with 22pF load capacitors (C7, C8). Provides a precise timebase for RTC functionality.

### J1 — USB-C Connector (UJ20-C-V-C-1-SMT-TR)

USB 2.0 Type-C receptacle (14-pin). CC1/CC2 have 5.1kΩ pull-downs (R6, R7) for device identification.

## Code Quick-Reference

```cpp
// --- Pin Definitions ---

// SPI LED output (directly usable with hardware SPI: VSPI)
#define PIN_LED_CS    17  // SPI chip select
#define PIN_LED_CLK   18  // SPI clock (VSPI CLK)
#define PIN_LED_DIN   23  // SPI MOSI (VSPI MOSI)

// Rotary encoder (active LOW, external 5.1k pull-ups)
#define PIN_ENC_A     26  // Encoder channel A
#define PIN_ENC_B     27  // Encoder channel B
#define PIN_ENC_SW    14  // Encoder push switch

// Audio output (PWM/DAC → PAM8403 amplifier)
#define PIN_AUDIO     25  // DAC1 output (IO25 is DAC channel 1)

// UART (directly to CH340C — used by USB serial, avoid reassigning)
// #define PIN_TXD0    1
// #define PIN_RXD0    3

// RTC crystal (do not use as GPIO)
// IO32 — XTAL 32.768kHz XIN
// IO33 — XTAL 32.768kHz XOUT
```

### Notes for Firmware

- **SPI**: IO17/IO18/IO23 map to the default VSPI bus (CLK=18, MOSI=23). IO17 is used as CS but is not the default VSPI CS0 (IO5), so configure it manually.
- **Audio**: IO25 is one of the ESP32's two DAC outputs (DAC1). You can use `dacWrite(25, value)` for simple waveforms, or configure I2S for higher quality audio output to the PAM8403.
- **Rotary encoder**: Use interrupt-driven decoding on IO26/IO27. The push button on IO14 should be debounced in software.
- **RTC crystal**: IO32/IO33 are dedicated to the 32.768kHz crystal. Use the ESP32's internal RTC with this external crystal for accurate timekeeping. Configure via `rtc_clk_32k_enable()` or the ESP-IDF RTC API.
- **Boot strapping**: IO0 is used for boot mode (auto-reset handles this). IO2 is NC. IO12/IO15 affect boot — if using them, ensure they're not pulled to unexpected states at power-on.
