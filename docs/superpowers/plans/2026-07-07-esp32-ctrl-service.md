# ESP32 A6ED Control Service Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace NUS + the Flutter phone app with the nRF bridge's `A6ED…` BLE control service on the ESP32, so the existing Garmin data field and ctrl app work directly against the ESP32.

**Architecture:** New `ctrl_svc.c` in `components/bridge_core/` — a NimBLE port of `boards/xiao-nrf52840/platform_ble_ctrl_svc.c` — takes over advertising from `nus_ctrl.c` (which is deleted along with `app/`). `LIST`/`STATUS` answer in compact `ctrl_frames` notifications; everything else goes through `ctrl_dispatch` with JSON to the console log. A new link callback in `machine.c` pushes an `'S'` frame on treadmill link changes.

**Tech Stack:** ESP-IDF 5.x / NimBLE, shared `bridge_core` C, Python `uv` + `bleak` for the mock.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-07-esp32-ctrl-service-design.md`
- Build env (local macOS): `export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.5_py3.13_env; export ESP_PYTHON=/opt/homebrew/bin/python3.13; source ~/esp/esp-idf/export.sh` then `./build.sh heltec-v3`
- UUIDs (128-bit, NimBLE little-endian byte order): service `A6ED0001-D344-460A-8075-B9E8EC90D71B`, control char `…0002…` (write/write-no-rsp), response char `…0003…` (notify). Little-endian base: `1B D7 90 EC E8 B9 75 80 0A 46 44 D3 xx xx ED A6` with bytes 12–13 = `01 00` / `02 00` / `03 00`.
- Watch apps filter scans on the 128-bit service UUID only — the GAP device name does NOT change (stays whatever `garmin_rsc.c` sets).
- Frames/grammar are the shared code in `ctrl_frames.h` / `ctrl_dispatch.h` — do not fork them.
- Do not port `connect_policy.c` / `last_device.c`; ESP32 keeps `machine_try_last()` semantics.
- `garmin_data_field/`, `garmin_ctrl_app/`, `boards/xiao-nrf52840/` are untouched.

---

### Task 1: `machine.c` — saved-device getter + link callback

**Files:**
- Modify: `components/bridge_core/machine.h`
- Modify: `components/bridge_core/machine.c`

**Interfaces:**
- Produces: `bool machine_saved_device(ftms_device_t *out)` — copy of the NVS-persisted last device, false if none. `void machine_set_link_cb(void (*cb)(bool connected))` — invoked from the adapter connect/disconnect event path.

- [ ] **Step 1: Add declarations to `machine.h`** after `machine_conn_rssi()`:

```c
/* Copy the NVS-persisted last-connected device into *out. False if none. */
bool machine_saved_device(ftms_device_t *out);

/* Register a callback fired when the treadmill link comes up or goes down
 * (used by ctrl_svc to push an unsolicited 'S' status frame). */
void machine_set_link_cb(void (*cb)(bool connected));
```

- [ ] **Step 2: Implement in `machine.c`.** Add near the top with the other statics: `static void (*s_link_cb)(bool);`. In `on_evt()`, invoke it: after the `save_last(...)`+log in the connected branch add `if (s_link_cb) s_link_cb(true);`; in the disconnected branch add `if (s_link_cb) s_link_cb(false);` immediately before the `machine_try_last();` call (after the early returns, so intentional protocol switches don't emit spurious downs). Add at the bottom:

```c
bool machine_saved_device(ftms_device_t *out) { return load_last(out); }

void machine_set_link_cb(void (*cb)(bool connected)) { s_link_cb = cb; }
```

- [ ] **Step 3: Build** `./build.sh heltec-v3` → exits 0.
- [ ] **Step 4: Commit** `git commit -m "feat(machine): saved-device getter and link-change callback"`

---

### Task 2: `ctrl_svc.c` — NimBLE control service + advertising

**Files:**
- Create: `components/bridge_core/ctrl_svc.h`
- Create: `components/bridge_core/ctrl_svc.c`
- Modify: `components/bridge_core/CMakeLists.txt` (add `"ctrl_svc.c"` to SRCS)

**Interfaces:**
- Consumes: `ctrl_dispatch()`, `ctrl_frame_device/list_end/status()`, `machine_get_devices()`, `machine_connected_device()`, `machine_saved_device()`, `garmin_rsc_on_gap_event()`.
- Produces (mirrors `nus_ctrl.h` shape so board wiring is a rename):

```c
#pragma once
#include <stdint.h>
void ctrl_svc_set_addr_type(uint8_t addr_type);
void ctrl_svc_register_gatt(void);   /* call before nimble_port_run() */
void ctrl_svc_start(void);           /* starts advertising */
void ctrl_svc_notify_status(void);   /* push 'S' frame if subscribed */
```

- [ ] **Step 1: Write `ctrl_svc.c`.** Port of the nRF glue onto the `nus_ctrl.c` NimBLE skeleton. Key content (single watch connection, `CTRL_LINE_MAX 64`):
  - Three `ble_uuid128_t` consts per the Global Constraints byte order.
  - GATT table: primary service; control char (`BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP`, access cb below); response char (`BLE_GATT_CHR_F_NOTIFY`, `val_handle = &s_rsp_handle`, read access returns `BLE_ATT_ERR_READ_NOT_PERMITTED`).
  - State: `static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE; static bool s_notify_on; static uint16_t s_rsp_handle; static uint8_t s_own_addr_type;` and a mutex-guarded TX queue `#define TXQ_LEN 12` of `{uint8_t len; uint8_t data[CTRL_FRAME_MAX];}` entries.
  - `txq_push()`: drop-new + `ESP_LOGW` when full, then `txq_pump()`. `txq_pump()`: while non-empty and `s_conn != NONE && s_notify_on`, build an mbuf via `ble_hs_mbuf_from_flat` and `ble_gatts_notify_custom(s_conn, s_rsp_handle, om)`; on `BLE_HS_ENOMEM` stop (resume from `BLE_GAP_EVENT_NOTIFY_TX`); other errors log-and-drop. If nobody is listening, clear the queue.
  - `send_status_frame()`: `const ftms_device_t *dev = machine_connected_device();` → `ctrl_frame_status(buf, dev != NULL, dev ? dev->proto : 0, dev ? dev->name : NULL)` → push.
  - `send_list_frames()`: `ftms_device_t devs[FTMS_MAX_DEVICES]; int n = machine_get_devices(devs, FTMS_MAX_DEVICES);` flags from `machine_connected_device()` addr match (`CTRL_DEV_FLAG_CONNECTED`) and `machine_saved_device()` addr match (`CTRL_DEV_FLAG_SAVED`); one `ctrl_frame_device` push per entry + `ctrl_frame_list_end`.
  - Write access cb: copy ≤64 bytes, NUL-terminate, strip trailing `\r\n`; `LIST` → `send_list_frames()`; `STATUS` → `send_status_frame()`; else `ctrl_dispatch(line, log_tx, NULL)` where `log_tx` does `ESP_LOGI(TAG, "ctrl: %s", msg)`.
  - Advertising (`start_advertising()`): copy of `nus_ctrl.c`'s — same params/intervals — with `fields.uuids128 = &CTRL_SVC_UUID`; scan response keeps RSC `0x1814` + GAP device name.
  - GAP event cb: forward every event to `garmin_rsc_on_gap_event(event)`. CONNECT: on failure restart advertising; on success record `s_conn`, reset `s_notify_on=false`, clear queue — do **not** restart advertising (single watch link; RSC/ctrl exclusivity by design). DISCONNECT: clear state, `start_advertising()`. SUBSCRIBE on `s_rsp_handle`: set `s_notify_on = cur_notify`, and when it turns on call `send_status_frame()` (greeting). NOTIFY_TX: `txq_pump()`.
  - Public API per the header above; `ctrl_svc_notify_status()` = if connected+subscribed, `send_status_frame()`.

  > **Amended after issue #15 / the notify-deadlock fix:** two details above
  > shipped broken and were corrected post-merge. (1) Advertising layout is
  > reversed from the plan: RSC `0x1814` + name go in the ADV packet and the
  > A6ED 128-bit UUID in the scan response — CIQ's `getServiceUuids()` only
  > surfaces scan-response UUIDs. (2) There is no `NOTIFY_TX: txq_pump()`
  > case: NimBLE raises `BLE_GAP_EVENT_NOTIFY_TX` synchronously inside
  > `ble_gatts_notify_custom()` (unlike the SoftDevice's deferred
  > `HVN_TX_COMPLETE`), so pumping there deadlocks on `s_tx_mutex`; stalled
  > frames are retried via a `ble_npl_callout` instead.
- [ ] **Step 2: Add `"ctrl_svc.c"` to `CMakeLists.txt` SRCS.**
- [ ] **Step 3: Build** `./build.sh heltec-v3` → exits 0 (service not yet wired; compiles standalone).
- [ ] **Step 4: Commit** `git commit -m "feat(bridge_core): NimBLE A6ED control service (port of nRF ctrl svc)"`

---

### Task 3: Wire boards to `ctrl_svc`, delete NUS

**Files:**
- Modify: `boards/heltec-v3/main/main.c` (`nus_ctrl_*` → `ctrl_svc_*`)
- Modify: `boards/heltec-v3/main/ui.c` (drop `nus_ctrl.h`/`nus_ctrl_push_state`; add `ctrl_svc.h`, `ctrl_svc_start()` in `ui_start()`, register link cb)
- Modify: `boards/xiao-c6/main/main.c` (same renames)
- Delete: `components/bridge_core/nus_ctrl.c`, `components/bridge_core/nus_ctrl.h`
- Modify: `components/bridge_core/CMakeLists.txt` (remove `"nus_ctrl.c"`)

**Interfaces:**
- Consumes: Task 1 `machine_set_link_cb`, Task 2 `ctrl_svc_*`.

- [ ] **Step 1: heltec-v3.** `main.c`: `nus_ctrl_set_addr_type` → `ctrl_svc_set_addr_type`, `nus_ctrl_register_gatt` → `ctrl_svc_register_gatt`, include swap. `ui.c`: include swap; delete `nus_ctrl_push_state(s)` from `on_state()`; `nus_ctrl_start()` → `ctrl_svc_start()`; in `ui_start()` before `machine_set_data_cb(on_state);` add:

```c
machine_set_link_cb(on_link);
```

with, next to `on_state()`:

```c
static void on_link(bool connected) {
    (void)connected;
    ctrl_svc_notify_status();   /* push 'S' frame to the watch */
}
```

- [ ] **Step 2: xiao-c6 `main.c`.** Same three renames + include swap; add the same `on_link` + `machine_set_link_cb(on_link)` next to its `on_state`/`machine_set_data_cb`.
- [ ] **Step 3: Delete NUS.** `git rm components/bridge_core/nus_ctrl.c components/bridge_core/nus_ctrl.h`; remove `"nus_ctrl.c"` from SRCS. Grep `nus_ctrl` repo-wide (excluding docs/, app/) → no code hits.
- [ ] **Step 4: Build both** `./build.sh heltec-v3` and `./build.sh xiao-c6` → exit 0.
- [ ] **Step 5: Commit** `git commit -m "feat(boards): watch control via A6ED ctrl service; remove NUS"`

---

### Task 4: Remove the Flutter app + CI/docs sweep

**Files:**
- Delete: `app/` (entire directory)
- Modify: `.github/workflows/ci.yml` (drop `build-android` job)
- Modify: `README.md`, `CLAUDE.md` (phone-app sections → direct-BLE description; both are the same doc family — `AGENTS.md`/`GEMINI.md` symlink to `CLAUDE.md`)

- [ ] **Step 1:** `git rm -r app/`; remove the `build-android` job from `ci.yml`.
- [ ] **Step 2: README.md** — replace phone-app mentions: the ESP32 path is now "watch BLE data field → ESP32 → treadmill" for control and RSC for data; note RSC-vs-control exclusivity ("disable the RSC sensor pairing on the watch to use the data field") and that the data field/ctrl app work on both bridge variants.
- [ ] **Step 3: CLAUDE.md** — update "Reverse Data Stream" section (drop phone hop, reference `ctrl_svc.c`, A6ED UUIDs, uppercase grammar, `'D'/'E'/'S'` frames), remove Flutter command block, remove `garmin_ciq_service.dart` mentions.
- [ ] **Step 4:** grep repo for `flutter\|garmin_ciq\|nus` in non-historical files → no stale references. Commit `git commit -m "chore: remove Flutter phone app; docs for direct watch control"`

---

### Task 5: `test/mock/mock_ctrl_watch.py`

**Files:**
- Create: `test/mock/mock_ctrl_watch.py` (executable, `uv` self-contained like siblings)
- Modify: `test/mock/README.md` (row in the table)

**Interfaces:**
- Consumes: the A6ED GATT contract from Task 2.

- [ ] **Step 1: Write the script.** Same `uv` header style as `test/mock/mock_watch.py` (`# /// script … dependencies = ["bleak"] …`). Behavior: scan for service `a6ed0001-d344-460a-8075-b9e8ec90d71b`; connect; subscribe to `…0003…` and decode/print frames (`'D'`: idx, rssi(int8), proto, flags, name; `'E'`: count; `'S'`: connected, proto, name); then read commands from stdin (`STATUS`, `LIST`, `SCAN`, `CONNECT <n>`, `SPEED <kmh>`, `INCLINE <pct>`, `STOP`) and write each line to `…0002…`. Decode example:

```python
def decode(frame: bytes) -> str:
    tag = chr(frame[0])
    if tag == "D":
        idx, rssi, proto, flags = frame[1], int.from_bytes(frame[2:3], "little", signed=True), frame[3], frame[4]
        name = frame[5:].split(b"\0")[0].decode(errors="replace")
        return f"D idx={idx} rssi={rssi} proto={'iFit' if proto else 'FTMS'} flags={flags:#04x} name={name!r}"
    if tag == "E":
        return f"E count={frame[1]}"
    if tag == "S":
        name = frame[3:].split(b"\0")[0].decode(errors="replace")
        return f"S connected={frame[1]} proto={'iFit' if frame[2] else 'FTMS'} name={name!r}"
    return f"? {frame.hex()}"
```

- [ ] **Step 2:** `chmod +x`, add README row: `mock_ctrl_watch.py | ctrl central | drive the A6ED control service like the watch data field / ctrl app`.
- [ ] **Step 3: Syntax check** `python3 -m py_compile test/mock/mock_ctrl_watch.py` → exits 0.
- [ ] **Step 4: Commit** `git commit -m "test(mock): watch-side driver for the A6ED control service"`

---

### Task 6: Verification

- [ ] **Step 1:** `make -C test/host` (host tests still pass; nothing shared changed semantically).
- [ ] **Step 2:** Rebuild both ESP32 boards from clean if any CMake edits landed after their last build.
- [ ] **Step 3:** Hardware smoke (requires user/hardware — flag as manual): flash heltec-v3; verify RSC pairing still works; verify data field connects and `SPEED` moves the belt; verify ctrl app `LIST`/`CONNECT`.
