# Project Instructions

## Build

Never build the project or run idf.py commands. The user will build and flash manually.
If something is needed from the toolchain (e.g., adding a component dependency), ask the user to do it.

## Code Style

- ESP-IDF v5.3.1 framework (not Arduino)
- Use tabs for indentation (width 4), per .clang-format
- K&R brace style

## Hardware

- ESP32-WROOM-32, 16MB flash
- Board schematic and GPIO map in HEDC-B002/README.md

## Firmware (HEDC-F001)

- Partition table: single 2MB factory app partition, ~14MB storage (LittleFS, spiffs subtype)
- Flash size must be set to 16MB in menuconfig
- webpage.html is compiled to webpage.h via webpage_compile.py (hooked into CMakeLists.txt)
- WiFi SoftAP: SSID "dotclock", password "dotclock1", IP 192.168.4.1
- MP3 decoding via chmorgan/esp-libhelix-mp3 component
- Audio output via dac_continuous driver (not legacy I2S) on GPIO25 (DAC1), 8-bit
