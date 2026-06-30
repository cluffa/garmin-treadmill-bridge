# garmin-ftms-sync-esp32

[![CI](https://github.com/cluffa/garmin-ftms-sync-esp32/actions/workflows/ci.yml/badge.svg)](https://github.com/cluffa/garmin-ftms-sync-esp32/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Firmware that bridges a Bluetooth treadmill to a Garmin watch and an Android phone. The device connects to a treadmill as a BLE central (FTMS or iFit), re-broadcasts live speed and distance to a Garmin watch as a native BLE RSC sensor, and exposes a USB serial interface so an Android phone can select the treadmill, monitor workout data, and send speed/incline control commands.

## Hardware

| Board | Target | Notes |
|-------|--------|-------|
| Seeed XIAO ESP32-C6 | `esp32c6` | Primary — USB-C, compact |
| Heltec WiFi LoRa 32 v3 | `esp32s3` | Has OLED display and battery management |

## Data Streams & Architecture

The system coordinates two independent data streams:

1. **Treadmill to Garmin (Implemented)**
   * `Treadmill (FTMS/iFit)` -> `ESP32 (BLE Central)` -> `Garmin Watch (BLE RSC Peripheral)`
   * Re-broadcasts live speed and distance to the watch.

2. **Garmin to Treadmill (Implemented)**
   * `Garmin Watch (ConnectIQ DataField)` -> `Phone App (BLE)` -> `ESP32 (BLE NUS)` -> `Treadmill (FTMS/iFit Control)`
   * Sends the current Garmin workout target pace to automatically control the treadmill's speed.
   * **Watch -> Phone**: ConnectIQ DataField reads `Activity.Info.currentWorkoutStep` for target pace; falls back to `currentSpeed`. Sends `workoutStatus` with `targetPace`, `targetPaceLow`, `targetPaceHigh` every 5s.
   * **Phone -> ESP32**: Phone app receives `workoutStatus` via BLE NUS, converts target pace (m/s → km/h), and sends a `speed` command to the ESP32.
   * **ESP32 -> Treadmill**: The ESP32 parses incoming `speed <x>` commands and writes the correct control characteristics to the treadmill.

## Repo layout

```
components/bridge_core/   — shared IDF component (parsers, BLE, serial ctrl)
boards/heltec-v3/         — Heltec board firmware (display, battery, buttons)
boards/xiao-c6/           — XIAO C6 board firmware (lean, USB serial only)
test/host/                — pure-C host tests (no hardware needed)
test/mock/                — Python mock treadmill and watch (macOS/Linux)
tools/serial_cli.py       — interactive USB serial CLI for the device
```

## Build

Requires [ESP-IDF v5.4+](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/).

```sh
# Build and flash XIAO C6
cd boards/xiao-c6
idf.py build flash monitor

# Build and flash Heltec v3
cd boards/heltec-v3
idf.py build flash monitor
```

## Host tests

```sh
cd test/host && make
```

## Serial CLI

```sh
uv run tools/serial_cli.py
```

Commands: `scan`, `list`, `connect <n>`, `speed <kmh>`, `incline <pct>`, `stop`, `status`

## License

MIT
