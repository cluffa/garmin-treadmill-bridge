# ESP32 control service — drop the phone, one data field for both bridges

**Date:** 2026-07-07
**Status:** Approved design, pending implementation plan

## Goal

Remove the Flutter phone app from the ESP32 bridge path. The watch talks to
the ESP32 directly, in one of two mutually exclusive ways chosen entirely on
the watch — no mode switch, no configuration on the ESP32:

- **RSC mode (forward data):** watch pairs the ESP32 as a native Running
  Speed & Cadence sensor. Unchanged from today.
- **Control mode (reverse control):** the Connect IQ BLE data field connects
  to the ESP32's control service and drives treadmill speed from structured
  workout targets. No belt speed is displayed or recorded on the watch in
  this mode (wrist pace covers recording).

Exclusivity is natural: the watch holds one BLE link to the ESP32, and once
either link is up the ESP32 stops advertising, so the other path can't
connect. The one user-facing rule: **disable the RSC sensor on the watch when
you want control mode**, or it will grab the connection first.

## Approach

Port the nRF52840 bridge's control service to the ESP32 (NimBLE), reusing the
shared `bridge_core` protocol code verbatim. The existing Garmin apps then
work against both bridge variants unchanged:

- `garmin_data_field/` (BLE data field, sends `SPEED <kmh>`)
- `garmin_ctrl_app/` (picker: `SCAN` / `LIST` / `CONNECT <n>` / `STATUS`)

NUS (`nus_ctrl.c`) and the Flutter app (`app/`) are removed.

## Components

### New: `ctrl_svc` — NimBLE GATT control service (ESP32 glue)

A NimBLE port of `boards/xiao-nrf52840/platform_ble_ctrl_svc.c`, living in
`bridge_core` next to `nus_ctrl.c`'s old slot (it is ESP32/NimBLE-specific
glue like `nus_ctrl` was; the platform-agnostic rule applies to protocol
logic, which stays in `ctrl_dispatch`/`ctrl_frames`).

Same GATT layout as the nRF bridge:

- Service `A6ED0001-D344-460A-8075-B9E8EC90D71B`
- Control char `A6ED0002-…` (write / write-no-rsp): uppercase `ctrl_dispatch`
  grammar lines (`SPEED 8.0`, `SCAN`, `LIST`, `CONNECT 2`, `STATUS`, `STOP`,
  `INCLINE 1.5`)
- Response char `A6ED0003-…` (notify): compact ≤20-byte frames from
  `ctrl_frames.h` (CIQ MTU is pinned at 23)

Command routing, mirroring the nRF glue:

- `LIST` → one `'D'` frame per `ftms_devlist` entry (flags mark connected
  device) + `'E'` end frame
- `STATUS` → one `'S'` frame (connected, proto, name)
- everything else → `ctrl_dispatch(line, tx→UART log, NULL)`; JSON replies go
  to the console only
- an `'S'` frame is pushed unsolicited when the treadmill link state changes
- notification greeting: on CCCD subscribe, push a `'S'` status frame

TX path: NimBLE's `ble_gatts_notify_custom` with a small queue analogous to
the nRF `s_txq` (NimBLE can also return EBUSY/ENOMEM under load). On queue
overflow, drop the new frame and log a warning — same as the nRF glue.

### Changed: advertising (currently owned by `nus_ctrl.c`)

Advertising ownership moves to `ctrl_svc` (or a small shared adv module).
Layout is today's with the NUS UUID swapped out:

- ADV packet: flags + 128-bit `A6ED0001…` service UUID
- Scan response: 16-bit `0x1814` (RSC) + device name

RSC `0x1814` stays discoverable for native sensor pairing; the data field and
ctrl app scan for the 128-bit ctrl UUID, exactly as they do against the nRF
bridge.

### Removed

- `components/bridge_core/nus_ctrl.c/.h` and its `SRCS` entry
- `app/` (entire Flutter project), its CI job, README/CLAUDE.md sections
- `garmin_ciq_service.dart` message contract documentation (dies with `app/`)

### Unchanged

- `garmin_rsc.c`, `machine_*`, `ctrl_dispatch`, `ctrl_frames`,
  `ftms_devlist`, `serial_ctrl` (serial CLI stays for debugging)
- ESP32 connect behavior: no port of `connect_policy.c` in this change; the
  ESP32 keeps its current scan/connect semantics driven by `SCAN`/`CONNECT`
  (and existing auto-reconnect in the machine layer)
- `garmin_data_field/` and `garmin_ctrl_app/`: zero changes; one `.prg` of
  each serves both bridge variants
- nRF52840 board: untouched

## Data flow after the change

Forward (RSC mode):
`Treadmill (FTMS/iFit)` → `ESP32 central` → `treadmill_state_t` →
`garmin_rsc` peripheral → watch native RSC pairing. (As today.)

Reverse (control mode):
`Watch workout step target` → CIQ data field → BLE write `SPEED <kmh>` to
`A6ED0002` → `ctrl_dispatch` → `machine_{ftms,ifit}_set_speed` → treadmill.
(The phone hop is gone.)

## Error handling

- Writes longer than the accept buffer (64 bytes, as nRF) are rejected/
  truncated with a log line.
- Notifications while unsubscribed or disconnected are dropped silently
  (nobody listening), matching nRF.
- `CONNECT <n>` with a stale index answers via `ctrl_dispatch`'s existing
  JSON error on the console; the watch app's timeout handles the silence —
  same behavior as against the nRF bridge.

## Testing

- Host tests: `ctrl_frames`/`ctrl_dispatch` already covered in `test/host`;
  no new host-testable logic is introduced (the new code is NimBLE glue).
- `test/mock/mock_watch.py` gains a sibling `mock_ctrl_watch.py` (BLE central
  that subscribes to `A6ED0003`, sends `STATUS`/`LIST`/`SPEED`, prints
  frames) so the control path can be exercised without the watch.
- Hardware smoke: data field on the fenix drives belt speed with the phone
  app uninstalled; ctrl app lists and switches devices; RSC pairing still
  works when the data field isn't connected.

## Out of scope

- Belt speed display/recording in control mode (explicitly not wanted)
- Dual simultaneous watch links (Approach C — rejected)
- Porting `connect_policy.c` / persisted last-device to ESP32
