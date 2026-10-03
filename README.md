# ESP32-C3 BLE Media Remote

A small ESP32-C3 project that turns a handful of buttons into a Bluetooth HID media controller for use with a computer, phone, or tablet.

This project is based on Espressif's Bluetooth HID example code and uses native ESP-IDF C APIs.

## Features

- Bluetooth HID media controls
- Media actions: volume up, volume down, next track, previous track, play/pause
- Optional SSD1306 OLED status display
- Simple GPIO-based input design using active-low buttons with internal pull-ups
- Built with ESP-IDF and designed for the ESP32-C3

## Hardware

Buttons are active-low with internal pull-ups:

- GPIO1: volume up
- GPIO0: volume down
- GPIO3: next track
- GPIO2: previous track
- GPIO4: play/pause

The optional 128x64 SSD1306 OLED uses I2C:

- SDA: GPIO8
- SCL: GPIO9
- VCC: 3.3V
- GND: GND
- Address: `0x3C`

The OLED displays `READY` at startup and the last action performed after each button press.

## Wiring Overview

The board is designed to work as a compact physical media remote. Each button connects to a GPIO configured as an input with an internal pull-up. When pressed, the input is pulled low and the code sends the matching HID media event over Bluetooth.

## Build and Flash

From an ESP-IDF terminal:

```powershell
idf.py set-target esp32c3
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with your serial port, such as `COM3` or `COM10`.

## Usage

1. Power the ESP32-C3 board.
2. Pair it as a Bluetooth keyboard/media device from your host computer or mobile device.
3. Press the configured buttons to send media control commands.
4. If connected, the OLED updates to show the last action performed.

## Project Structure

- `main/app_main.c` — main application entry point
- `main/esp_hid_gap.c` — Bluetooth GAP/HID setup and scan logic
- `main/esp_hid_gap.h` — project declarations and BLE/BT definitions
- `src/` — additional project source files
- `CMakeLists.txt` — build configuration

## License and Attribution

This project is based on Espressif's `examples/bluetooth/esp_hid_device` demo and retains the original Espressif copyright and SPDX notices in the source files.

The upstream files in this project include SPDX license headers indicating `Unlicense OR CC0-1.0` for those portions. Please keep those notices intact when reusing or redistributing the code.