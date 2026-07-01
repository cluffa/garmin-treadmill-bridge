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

# Garmin Connect IQ data field (requires Connect IQ SDK + developer key)
# CI uses blackshadev/garmin-connectiq-build-action; locally use monkeyc CLI:
# monkeyc -f garmin_data_field/monkey.jungle -d fenix8solar51mm -o app.prg
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

If a `boards/*/build` dir has a stale target, `battery.c` fails to compile
(`adc_cali_create_scheme_curve_fitting` is S3-only). Fix: `idf.py set-target esp32s3`.

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
