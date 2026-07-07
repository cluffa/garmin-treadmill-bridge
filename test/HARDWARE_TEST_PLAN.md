# Hardware Test Plan

Covers the two boards (XIAO C6, Heltec v3) and the full data path:
treadmill → ESP32 → Garmin watch (RSC mode), plus the reverse path:
watch data field → ESP32 control service → treadmill (control mode).
The watch holds one link to the bridge, so the two modes are tested
separately — disable the RSC sensor pairing on the watch before control-mode
tests.

**Prerequisites:** ESP32 flashed, treadmill powered on and idle, Garmin watch paired to nothing, the CIQ data field + ctrl app `.prg`s installed, `tools/serial_cli.py` available.

---

## 1. Firmware build smoke test

Run before touching hardware.

```sh
cd test/host && make
```

All host-unit tests pass. If not, stop — the parsers are broken.

---

## 2. Serial CLI basics

Goal: confirm the ESP32 boots and the serial interface responds.

```sh
./tools/serial_cli.py          # auto-detects port
```

| Step | Command | Expected |
|------|---------|----------|
| 2.1 | `status` | Prints state (idle / scanning / connected) without crashing |
| 2.2 | `scan` | Begins BLE scan; log shows nearby devices within ~10 s |
| 2.3 | `list` | Prints numbered device list including the treadmill |

---

## 3. FTMS treadmill connection (forward path, step 1)

Goal: bridge connects to the treadmill and receives data.

| Step | Action | Expected |
|------|--------|----------|
| 3.1 | `connect <n>` (treadmill index) | Log shows "Connected to …" and FTMS service found |
| 3.2 | Start treadmill at any speed | Log shows periodic speed/incline values updating |
| 3.3 | Change speed on treadmill console | Values in serial log change within ~1 s |
| 3.4 | `status` | Reports connected device name, current speed, incline |

If the treadmill is iFit (`I_TL`): allow ~9 s for the 18-command init sequence before data flows.

---

## 4. RSC peripheral (forward path, step 2)

Goal: Garmin watch sees and pairs to the bridge's RSC sensor.

| Step | Action | Expected |
|------|--------|----------|
| 4.1 | On watch: scan for speed/cadence sensors | `garmin-ftms-sync` appears |
| 4.2 | Pair and start an activity | Watch shows live speed matching treadmill console |
| 4.3 | Change treadmill speed | Watch speed updates within ~2 s |
| 4.4 | Run `./test/mock/mock_watch.py` on Mac (treadmill connected) | Terminal prints decoded RSC notifications with correct speed/distance |

**Sanity check:** speed on watch should match `status` output on serial CLI.

---

## 5. Control service basics (reverse path, no watch needed)

Goal: the A6ED control service accepts commands and answers in compact frames.

| Step | Action | Expected |
|------|--------|----------|
| 5.1 | Run `./test/mock/mock_ctrl_watch.py` on Mac | Connects, subscribes; prints an `'S'` status frame greeting |
| 5.2 | Type `LIST` | One `'D'` frame per discovered treadmill + `'E'` end frame |
| 5.3 | Type `SPEED 8.0` | Serial log shows the dispatch; treadmill adjusts |
| 5.4 | Type `INCLINE 1.5` | Treadmill incline changes |
| 5.5 | Type `STOP` | Treadmill decelerates to 0 |
| 5.6 | Power-cycle the treadmill link (or `CONNECT <n>`) | Unsolicited `'S'` frame arrives on the link change |

---

## 6. Garmin workout target pace → treadmill (reverse path, full loop)

Goal: active Garmin workout with a pace target automatically drives treadmill speed.

| Step | Action | Expected |
|------|--------|----------|
| 6.1 | Install the ConnectIQ DataField on the watch (`.prg` via MTP or Garmin Express) | DataField appears in available fields |
| 6.2 | Create a structured workout with a speed-target interval (e.g. 10 km/h) | — |
| 6.3 | Disable the RSC sensor pairing on the watch; start the workout with the DataField active | DataField finds the bridge over BLE and shows the link as up |
| 6.4 | Enter the speed-target interval | Treadmill adjusts to the target within ~5 s (DataField sends every 5 s) |
| 6.5 | Skip to a different pace interval | Treadmill adjusts to new target |
| 6.6 | End workout / no active step | Treadmill stays at last commanded speed (no spurious stops) |

**What to check in logs:**
- Serial CLI `status` shows speed matching the workout target (× 3.6 for m/s → km/h).
- ESP32 console shows `ctrl_svc: rx "SPEED …"` lines every ~5 s while a speed-target step is active.

---

## 7. Heltec v3 board-specific

Only relevant if testing the Heltec board.

| Step | Action | Expected |
|------|--------|----------|
| 7.1 | Boot with OLED connected | Display shows status / speed on screen |
| 7.2 | Connect to treadmill | Display updates with live data |
| 7.3 | Confirm battery management doesn't interfere with BLE | Connection stable for ≥10 min continuous run |

---

## 8. Reconnect / edge cases

| Step | Scenario | Expected |
|------|----------|----------|
| 8.1 | Power-cycle treadmill while connected | Bridge detects disconnect; auto-reconnects within ~30 s |
| 8.2 | Move watch out of BLE range then back | RSC re-subscribes; data resumes |
| 8.3 | Send `connect` while already connected to a different device | Bridge tears down old connection first (no dual-machine state) |
| 8.4 | Data field BLE disconnect → reconnect mid-workout | Bridge re-advertises; control resumes; no stale speed command applied |

---

## Quick reference: useful commands during testing

```sh
# Serial CLI
./tools/serial_cli.py
# commands: scan / list / connect <n> / speed <kmh> / incline <pct> / stop / status

# Watch RSC output
./test/mock/mock_watch.py

# Observe both RSC and FTMS side by side
./test/mock/sensor_logger.py

# Deploy .prg to watch over MTP (get parent_id from mtp-folders first)
./tools/mtp_send_to_folder garmin_data_field/out/app.prg app.prg <parent_id>
```

Filter NimBLE verbose logs when reading serial output:
```sh
idf.py monitor | grep -v "att_handle=14"
```
