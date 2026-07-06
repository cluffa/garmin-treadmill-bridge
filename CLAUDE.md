# CLAUDE.md

> **Note to agents:** `AGENTS.md` and `GEMINI.md` are symlinks to this file. Always edit `CLAUDE.md` to update instructions.

Project-specific notes that aren't obvious from the README or code.

## Commands

```sh
# Firmware — build or build+flash
./build.sh heltec-v3               # build only
./build.sh heltec-v3 flash         # build + flash (auto-detects port)
./build.sh heltec-v3 flash /dev/cu.usbserial-0001

# Flutter phone app
cd app && flutter run              # run on connected device
cd app && flutter build apk        # release APK

# Garmin Connect IQ apps (requires Connect IQ SDK + developer key)
# CI uses blackshadev/garmin-connectiq-build-action; locally use monkeyc CLI:
# monkeyc -f garmin_data_field/monkey.jungle -d fenix8solar51mm -o app.prg
# monkeyc -f garmin_ctrl_app/monkey.jungle   -d fenix8solar51mm -o ctrl.prg
# monkeyc lives in "~/Library/Application Support/Garmin/ConnectIQ/Sdks/<sdk>/bin";
# key: ~/Documents/garmin_developer_key.der
```

## Building (local macOS quirk)

The ESP-IDF Python venv here is for **3.13**, but the system `python3` is 3.14, so
a plain `source ~/esp/esp-idf/export.sh` fails ("virtual environment … not found").
Pin the interpreter first:

```sh
export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.5_py3.13_env
export ESP_PYTHON=/opt/homebrew/bin/python3.13
source ~/esp/esp-idf/export.sh
```

## Targets

| Board | dir | target |
|-------|-----|--------|
| Heltec WiFi LoRa 32 v3 | `boards/heltec-v3` | **`esp32s3`** |
| Seeed XIAO ESP32-C6 | `boards/xiao-c6` | `esp32c6` |
| Seeed XIAO nRF52840 | `boards/xiao-nrf52840` | nRF52840 + **S340** (not ESP-IDF — see below) |

If a `boards/*/build` dir has a stale target, `battery.c` fails to compile
(`adc_cali_create_scheme_curve_fitting` is S3-only). Fix: `idf.py set-target esp32s3`.

## xiao-nrf52840 (ANT+ bridge — different toolchain)

This board removes the phone from the loop: BLE central → treadmill, **ANT+
SDM footpod** → watch (native sensor, no BLE slot), BLE peripheral
`TMILL-CTRL` ← data field target-pace writes. Design/plan:
`docs/superpowers/specs/2026-07-01-nrf52840-ant-bridge-design.md`.

Builds with **nRF5 SDK v17.1.0 + S340 SoftDevice v7.0.1**, NOT ESP-IDF/build.sh:

```sh
# prerequisites: arm-none-eabi-gcc, nrfjprog, nRF5 SDK, S340 (from thisisant.com)
make -C boards/xiao-nrf52840 SDK_ROOT=~/nRF5_SDK_17.1.0_ddde560 \
     S340_API=~/s340/API/include
make -C boards/xiao-nrf52840 flash_softdevice   # once
make -C boards/xiao-nrf52840 flash              # app, over SWD (J-Link on back pads)
```

**On this machine** the working invocation (PlatformIO's ARM GCC; S340 in
~/Downloads) is:

```sh
make -C boards/xiao-nrf52840 -j8 \
  GNU_INSTALL_ROOT=$HOME/.platformio/packages/toolchain-gccarmnoneeabi/bin/ \
  GNU_VERSION=7.2.1 \
  S340_API=$HOME/Downloads/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.API/include
```

Gotchas:
- **S340 is NOT in the nRF5 SDK** — it's licensed via thisisant.com (free
  registration). Same for the **ANT+ network key**: copy
  `ant_network_key.h.example` → `ant_network_key.h` (git-ignored) and fill it
  in, or the build fails at the `#include`.
- `sdk_config.h` is the unmodified SDK template from
  `examples/multiprotocol/ble_ant_app_hrm/pca10056/s340/config/`; all project
  overrides live in `app_config.h` (`USE_APP_CONFIG`). Copy the template in on
  first build.
- First build: verify FLASH origin (S340 APP_CODE_BASE) and RAM origin in
  `xiao_nrf52840_s340.ld` — mismatch logs the required RAM start over RTT.
- Flashing needs SWD (the XIAO's UF2 bootloader can't write the SoftDevice
  region reliably); logs are SEGGER RTT, not UART.
- **`components/bridge_core/` must stay platform-agnostic** — no SoftDevice/
  nRF includes there. nRF glue lives in `boards/xiao-nrf52840/` only
  (`ifit_poll.c` there is the extracted, host-tested iFit FSM; frames verbatim
  from `machine_ifit.c`).
- Control writes from the watch use the **uppercase** `ctrl_dispatch`
  grammar (`SPEED 8.0`, `SCAN`, `CONNECT 2`, `STOP`), service UUID
  `A6ED0001-…`, control char `A6ED0002-…`, response char `A6ED0003-…`
  (notify). `LIST`/`STATUS` answer on the response char in the compact
  ≤20-byte `'D'/'E'/'S'` frames from `ctrl_frames.h` (Garmin CIQ's MTU is
  pinned at 23 — a JSON line doesn't fit one notification); other commands'
  JSON replies go to RTT only. An `'S'` frame is also pushed when the
  treadmill link changes.
- **Treadmill choice:** the firmware scans both protocols into a device
  list and connects per `connect_policy.c` — the fds-persisted
  last-connected device the moment it's seen, else the strongest-RSSI
  device after a 6 s window (15 s hold-out when a saved device exists but
  isn't visible yet). `garmin_ctrl_app/` (a CIQ device app, distinct from
  the data field) lists the devices and overrides with `CONNECT <n>`; the
  pick becomes the new saved device. Only one watch peripheral link exists
  (`NRF_SDH_BLE_PERIPHERAL_LINK_COUNT=1`), so the picker app and the data
  field can't both be connected — use the app outside activities.
- The ANT license: the Makefile passes the **evaluation** `ANT_LICENSE_KEY`
  (personal use). The ANT+ **network key** in `ant_network_key.h` is
  separate — a zeroed placeholder builds but Garmin watches won't hear the
  footpod; put the real thisisant.com key back after re-cloning.

## Tools

### `tools/serial_cli.py` — serial command interface

Interactive CLI over UART0 for driving the ESP32 without touching hardware.
Auto-detects the port or takes one as an argument.

```sh
./tools/serial_cli.py                     # auto-detect
./tools/serial_cli.py /dev/cu.usbserial-0001
# commands: scan / list / connect <n> / speed <kmh> / incline <pct> / status
```

Requires `uv` (self-contained script, fetches `pyserial` automatically).

### `tools/mtp_send_to_folder.c` — deploy .prg to watch over MTP

The standard `mtp-sendfile` CLI sends to MTP root, which Garmin ignores — apps
must live in `GARMIN/Apps`. This tool targets a specific parent folder ID.

```sh
# Build (requires libmtp: brew install libmtp)
cc tools/mtp_send_to_folder.c -I/opt/homebrew/include -L/opt/homebrew/lib \
   -lmtp -o tools/mtp_send_to_folder

# Usage: find the GARMIN/Apps folder id first (mtp-folders), then:
./tools/mtp_send_to_folder garmin_data_field/out/app.prg app.prg <parent_id>
```

### `test/mock/` — BLE test harnesses

Python scripts for exercising the bridge without real hardware. See
`test/mock/README.md` for full details.

| Script | Role | Use for |
|--------|------|---------|
| `mock_treadmill.py` | FTMS peripheral | stand-in treadmill; accepts CP speed/incline writes |
| `mock_watch.py` | RSC central | verify bridge's RSC peripheral output end-to-end |
| `sensor_logger.py` | RSC+FTMS central | log all nearby sensors to console + CSV |

All are self-contained `uv` scripts (no venv needed):

```sh
./test/mock/mock_treadmill.py
./test/mock/mock_watch.py
./test/mock/sensor_logger.py
```

## Debugging without the button

UART0 (`/dev/cu.usbserial-0001` on the Heltec) is **both** the console log and the
serial command interface (`serial_ctrl.c`): `SCAN` / `LIST` / `CONNECT <idx>` /
`STATUS` / `SPEED` / `INCLINE` / `STOP`, plus pushed `{"event":"state",…}` JSON.
Drive a device switch and read live incline/speed over this port without touching
the hardware. NimBLE GATT verbose logs (iFit keepalive writes to `att_handle=14`)
flood this port — filter them when reading.

## Architecture invariant: one machine connection at a time

`machine_ftms` and `machine_ifit` are separate BLE-central adapters but feed **one
shared state callback** (`machine_set_data_cb`). They can each hold an independent
connection, so if both are connected the two notification streams clobber each
other in the shared state — e.g. an FTMS sensor's incline showing up under an iFit
label. `machine_connect()` therefore tears down the *other* protocol
(`machine_{ftms,ifit}_disconnect()`, which also cancels an in-flight connect)
before connecting, and `on_evt()` suppresses auto-reconnect while the other
protocol is connecting/connected. **Do not reintroduce simultaneous connections.**

## Forward Data Stream (Treadmill -> Garmin)

The primary implemented data stream broadcasts treadmill metrics to the watch:
* `Treadmill (FTMS/iFit)` -> `ESP32 (BLE Central)` -> `Garmin Watch (BLE RSC Peripheral)`
* The ESP32 connects to the treadmill, parses its proprietary or standard data frames into a generic `treadmill_state_t` via `bridge_core`, and then `garmin_rsc.c` handles exposing it as a standard BLE Running Speed and Cadence (RSC) sensor that any Garmin watch can pair with.

## Reverse Data Stream (Garmin -> Treadmill)

Automatically controls treadmill speed based on Garmin workout pace targets: `Garmin Watch (ConnectIQ)` -> `Phone App` -> `ESP32` -> `Treadmill`. Fully implemented.

1. **Watch:** DataField reads `Activity.Info.currentWorkoutStep` for target pace (m/s); falls back to `currentSpeed`. Sends `workoutStatus` with `targetPace`, `targetPaceLow`, `targetPaceHigh` every 5s.
2. **Phone App:** `garmin_ciq_service.dart` handles `workoutStatus` messages — converts target pace from m/s to km/h and calls `_bridge.setSpeed()`; exposes `targetSpeedLowKmh`/`targetSpeedHighKmh` getters.
3. **Phone -> ESP32:** Flutter app uses BLE NUS to send control strings (e.g. `speed 10.0`) to the ESP32.
4. **ESP32 -> Treadmill:** `nus_ctrl.c` and `ctrl_dispatch.c` parse incoming speed commands and apply them to the treadmill via FTMS/iFit.


## iFit / NordicTrack 6.5S (I_TL) protocol

The treadmill model is **NordicTrack 6.5S** (T6.5S v81), BLE name `I_TL`.

Requires an **18-command init sequence** on write char `0x1534` before it streams,
then a **6-phase keepalive poll** — both implemented in `machine_ifit.c` via the
`s_poll_step` state machine (init takes ~9s, then poll cycles). Source:
`qdomyos-zwift/proformtreadmill.cpp` (`nordictrack_t65s_treadmill` variant).

Notification frame format:
- Header: bytes 0-3 = `00 12 01 04`
- Speed: `(b[10] | b[11]<<8) / 100` → km/h
- Incline: signed `(b[12] | b[13]<<8) / 100` → %
- Frame length: exactly 20 bytes

### Outgoing control (SPEED/INCLINE/STOP over serial/NUS)

`machine_ifit_set_speed`/`_set_incline`/`_stop` (`machine_ifit.c`) queue a request
(`s_req_speed`/`s_req_incline`) rather than writing immediately. **Control writes
must be injected at a specific phase of the 6-phase keepalive poll, not fired
ad hoc** — the treadmill firmware silently ignores writes sent outside that slot.
This was confirmed on hardware: an immediate write did nothing, while injecting at
the right phase moved the belt.

- Speed/incline writes go out at **poll phase 2**, immediately after the
  `POLL2`/`noOpData3` frame — matches `qdomyos-zwift`'s `case 2` in its own poll
  switch. Frame: `{0xff,0x0d,0x02,0x04,0x02,0x09,0x04,0x09,0x02,0x01,<kind>,<lo>,<hi>,0,0,0,0,0,0,0}`
  preceded by a `{0xfe,0x02,0x0d,0x02}` no-op, `kind` = `0x01` speed / `0x02`
  incline, value ×100 little-endian in bytes 11–12, checksum byte 14 =
  `lo_byte + 0x12` (the `nordictrack_t65s_treadmill` branch's formula — see
  `forceSpeed`/`forceIncline` in `proformtreadmill.cpp`).
- **`forceSpeed` only changes speed while the belt is already moving.** From a
  dead stop it does nothing — you must first send the 5-frame `START_SEQ`
  (`nordictrack_t65s_treadmill_81_miles` variant) at **poll phase 5**, right
  after the `POLL5`/`noOpData6` frame. `machine_ifit.c` tracks the last decoded
  speed (`s_cur_speed_kmh`) from notifications and auto-fires `START_SEQ` when a
  nonzero speed is requested while stopped; the pending speed request is then
  applied once the belt is confirmed moving.
- Known side effect, not something we send deliberately: any BLE speed command
  (belt-start or plain speed change) makes the treadmill's own console display a
  "3:00" countdown. Confirmed via GATT write logs that this happens even for a
  plain phase-2 `forceSpeed` with no `START_SEQ` involved, so it's inherent
  console behavior for BLE/manual-mode control, not a bug in our frames.
  Harmless; unexplored whether any frame field can suppress it.
- Hardware-verified end to end: belt at 13.19 km/h → 7.8 → 8.0 km/h speed
  changes while moving, and a full restart from a stopped belt (0 → 9.66 km/h)
  via `START_SEQ`.
