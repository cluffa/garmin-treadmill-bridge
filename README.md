# garmin-treadmill-bridge

[![CI](https://github.com/cluffa/garmin-treadmill-bridge/actions/workflows/ci.yml/badge.svg)](https://github.com/cluffa/garmin-treadmill-bridge/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Firmware that bridges a Bluetooth treadmill to a Garmin watch — no phone involved. The device connects to a treadmill as a BLE central (FTMS or iFit) and offers the watch two mutually exclusive links, chosen entirely on the watch: pair it as a native BLE RSC sensor (live speed/distance), or connect the Connect IQ data field to its control service so structured-workout target paces drive the belt. A USB serial CLI covers setup and debugging.

## Hardware

| Board | Target | Notes |
|-------|--------|-------|
| Seeed XIAO ESP32-C6 | `esp32c6` | Primary — USB-C, compact |
| Heltec WiFi LoRa 32 v3 | `esp32s3` | Has OLED display and battery management |
| Seeed XIAO nRF52840 | nRF52840 + S340 | ANT+ bridge variant — both streams at once (see below) |

## Data Streams & Architecture

The ESP32 boards offer two data streams; the watch holds a single BLE link to the bridge, so you use one at a time (whichever connects first wins — disable the RSC sensor pairing on the watch when you want control mode):

1. **RSC mode — treadmill data to the watch**
   * `Treadmill (FTMS/iFit)` -> `ESP32 (BLE Central)` -> `Garmin Watch (BLE RSC Peripheral)`
   * Pair the bridge as a running sensor; live belt speed and distance are recorded in the activity.

2. **Control mode — workout targets to the treadmill**
   * `Garmin Watch (ConnectIQ DataField, BLE)` -> `ESP32 control service` -> `Treadmill (FTMS/iFit Control)`
   * The data field reads the structured workout's target pace and writes `SPEED <kmh>` to the bridge's `A6ED0001-…` control service — the same service the nRF52840 variant exposes, so one data field build works with either bridge.
   * The `garmin_ctrl_app` Connect IQ app can also connect (outside activities) to list discovered treadmills and switch with `CONNECT <n>`.

### nRF52840 ANT+ bridge (both streams at once)

The `boards/xiao-nrf52840` build runs both streams simultaneously by moving the data stream off BLE:

* **Forward**: `Treadmill (FTMS/iFit)` -> `nRF52840 (BLE Central)` -> **ANT+ SDM footpod** -> watch (native sensor, no BLE profile slot used).
* **Reverse**: `Garmin DataField` -> `nRF52840 "TMILL-CTRL" (BLE)` -> treadmill — the workout target pace goes straight from the watch to the belt.
* **Treadmill choice**: auto-connects to the last-used treadmill, else the closest (strongest RSSI); the `garmin_ctrl_app` Connect IQ app lists what the bridge can see and lets you pick.

Requires the nRF5 SDK 17.1.0 + S340 SoftDevice and an ANT+ network key — build details in `CLAUDE.md`.

## Repo layout

```
components/bridge_core/   — shared platform-agnostic core (parsers, policy, ctrl)
boards/heltec-v3/         — Heltec board firmware (display, battery, buttons)
boards/xiao-c6/           — XIAO C6 board firmware (lean, USB serial only)
boards/xiao-nrf52840/     — nRF52840 ANT+ bridge firmware (nRF5 SDK + S340)
garmin_data_field/        — Connect IQ data field (target pace -> bridge)
garmin_ctrl_app/          — Connect IQ app (treadmill picker / bridge status)
test/host/                — pure-C host tests (no hardware needed)
test/mock/                — Python mock treadmill and watch (macOS/Linux)
tools/serial_cli.py       — interactive USB serial CLI for the device
```

## Flash from your browser

No toolchain needed for the ESP32 boards — open the flasher page in Chrome or
Edge, plug the board into USB, and click flash:

**→ https://cluffa.github.io/garmin-treadmill-bridge/**

It uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/) (Web Serial)
to write the latest [`nightly`](https://github.com/cluffa/garmin-treadmill-bridge/releases/tag/nightly)
merged firmware image straight to the chip. Source for the page lives in
`docs/flasher/`. The nRF52840 variant isn't web-flashable (SWD only).

The same page can also **sideload the Connect IQ app to the watch** over USB
(WebUSB/MTP) — plug the watch in and click *Install data field* / *Install
picker*; the latest `.prg` is fetched from the nightly release and dropped into
`GARMIN/Apps` (or send your own `.prg`). The MTP client (`docs/flasher/mtp.js`)
is a fork of [webmtp](https://github.com/tidepool-org/webmtp) with an upload
path added; it works where the OS doesn't claim the MTP interface (macOS, most
Linux). Prebuilt apps target `fenix8solar51mm`.

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
