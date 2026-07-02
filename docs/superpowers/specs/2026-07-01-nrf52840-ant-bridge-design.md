# nRF52840 ANT+ Bridge — Design

**Date:** 2026-07-01
**Status:** Approved design, pre-implementation
**Scope:** nRF52840 firmware **+** ConnectIQ data field rewrite

## Problem

Garmin watches will not let a single BLE device be paired as a **system sensor**
(RSC) *and* be connected via a ConnectIQ data field's BLE (`Toybox.BluetoothLowEnergy`)
channel at the same time. The current ESP32 design works around this by tunneling
the reverse control path through a phone:

```
Treadmill ─BLE(FTMS/iFit)→ ESP32 ─BLE(RSC sensor)→ Watch          (forward: speed)
                              ▲                                     (reverse: target pace)
Watch(data field) → Phone app(BLE NUS) → ESP32 → Treadmill
```

The phone exists solely to dodge the sensor/data-field BLE collision.

## Goal

Replace the ESP32 with an **nRF52840**, which can run **BLE and ANT concurrently**.
Move the speed relay to **ANT+** (a native footpod sensor) so the watch's only BLE
link is the data field's — eliminating the collision and the phone entirely.

```
Treadmill ─BLE(FTMS/iFit)→ nRF52840 ─ANT+ (SDM footpod)→ Watch (native sensor)
                              ▲   └─BLE(GATT peripheral)── Watch data field (target pace)
                              └─ controls belt from target pace
```

The nRF52840 wears three concurrent radio hats: BLE **central** (treadmill),
BLE **peripheral** (data field control), and ANT **master** (speed broadcast).

## Key decisions (locked during brainstorming)

| Decision | Choice | Rationale |
|---|---|---|
| Scope | Firmware + data field | The two independent halves that make the topology work end-to-end. Flutter app retirement is out of scope (left in repo, out of the loop). |
| Stack | **nRF5 SDK + S340 SoftDevice** | S340 is the only concurrent BLE+ANT SoftDevice. Nordic's ANT+ SDM examples target exactly nRF5 SDK + S340. Plain C, closest to current code style. Zephyr/nRF Connect SDK has essentially no ANT support. |
| Treadmill protocols | **Both FTMS + iFit** | Preserve the generic adapter and support for standard FTMS treadmills, not just the owned NordicTrack 6.5S. |
| Data field role | **Send-only target paces** | Speed now reaches the watch natively over ANT+, so the field is a pure CIQ BLE client. No readback, minimal UI. |
| Board | **Seeed XIAO nRF52840** | Parallels the existing XIAO ESP32-C6. Headless bring-up (SEGGER RTT / UART logging); no OLED/battery-gauge/UI initially. |
| Repo layout | **Approach A** — new board dir, shared `bridge_core` | Maximizes reuse of already-portable protocol code; one repo, one source of truth. |

## Repository layout (Approach A)

```
components/bridge_core/          # SHARED, platform-agnostic — reused as-is
boards/xiao-nrf52840/            # NEW nRF5 SDK firmware (Make + SoftDevice)
  ├─ platform_ble_central.{c,h}  #   S340 GATT-client glue for machine_{ftms,ifit}
  ├─ platform_ant_sdm.{c,h}      #   ANT+ Stride SDM master (replaces garmin_rsc)
  ├─ platform_ble_ctrl_svc.{c,h} #   S340 GATT-server control service (replaces nus_ctrl)
  ├─ ant_sdm_encode.{c,h}        #   pure SDM page encoder (host-testable)
  ├─ main.c                      #   app bring-up, RTT logging, wiring
  ├─ sdk_config.h, Makefile, linker/SoftDevice glue
garmin_data_field/               # REWRITTEN: CIQ BLE client, phone path removed
```

Two build systems coexist (ESP-IDF for the ESP boards, nRF5 SDK Make for the
XIAO). This is documented in `CLAUDE.md` as part of the work.

## `bridge_core` reuse boundary

Reused **unchanged** (already have zero ESP/NimBLE includes; host-tested today):

- `model.h` — `treadmill_state_t`
- `ftms_parse.c`, `ifit_parse.c` — frame decoding
- `ctrl_dispatch.c` — ASCII command parse/dispatch (`ctrl_dispatch()`)
- `ftms_devlist.c` — merged FTMS+iFit device list
- The iFit protocol state machine logic in `machine_ifit.c` (18-command init,
  6-phase keepalive, `forceSpeed`/`START_SEQ` frame builders, checksums)

**Rewritten** (were NimBLE/ESP-coupled):

- `machine.c`, `machine_ftms.c`, `machine_ifit.c` transport layers → the *frame
  builders and FSMs stay*, but `sd_ble_gattc_*` calls replace NimBLE GATT.
  The **one-connection-at-a-time invariant** (tear down the other protocol before
  connecting; suppress auto-reconnect while the other is connecting) carries over
  verbatim — it is protocol logic, not transport.
- `garmin_rsc.c` → deleted for this board; replaced by `platform_ant_sdm` +
  `ant_sdm_encode`. **`rsc_encode.c` is NOT reused** — it encodes the BLE RSC
  Measurement characteristic; ANT+ SDM uses a different page format. The
  speed/distance math concept transfers; the byte layout is new.
- `nus_ctrl.c` → replaced by `platform_ble_ctrl_svc` (a GATT server the data
  field writes to). Same `ctrl_dispatch()` sink underneath.
- `serial_ctrl.c` → optional debug console over RTT/UART, thin re-wire of
  `ctrl_dispatch()`.

## Firmware components

### `platform_ble_central` — treadmill link (BLE central)
S340 GATT client. Scans for FTMS (`0x1826`) and iFit (`0x1533`) service UUIDs,
connects, subscribes to notifications, and pumps raw frames into `ftms_parse` /
`ifit_parse`. Owns write characteristics for control (FTMS Control Point; iFit
`0x1534`). Preserves the shared `machine_state_cb` → single `treadmill_state_t`
sink and the one-connection invariant. iFit control writes must still be injected
at the correct keepalive poll phase (phase 2 for speed/incline, phase 5 for
`START_SEQ`) — timing owned by the existing FSM, now driven off S340 events.

### `platform_ant_sdm` — speed relay (ANT master)
Opens an ANT master channel advertising as an ANT+ **Stride-Based Speed &
Distance Monitor** (device type 124). Periodically transmits SDM data pages
built by `ant_sdm_encode` from the latest `treadmill_state_t` (instantaneous
speed from `speed_mps`, cumulative distance from `distance_m`). Uses the ANT+
network key (see Licensing). The watch pairs it as a native footpod, getting
speed/pace without any BLE involvement.

### `ant_sdm_encode` — pure SDM page encoder
Stateless function mirroring `rsc_encode`'s testability:
`ant_sdm_encode_page(const treadmill_state_t*, uint8_t page, uint8_t out[8])`.
Produces the 8-byte ANT+ SDM data pages (page 1 distance/speed, page 2
cadence/status, etc.) needed for a valid footpod. Host-testable with no radio.

### `platform_ble_ctrl_svc` — control receive (BLE peripheral)
Small S340 GATT **server** with a single writable characteristic. The data field
connects as a CIQ BLE central and writes an ASCII/target-pace payload; the
handler feeds it into `ctrl_dispatch()`, which drives `machine_set_speed()` etc.
Advertises alongside the ANT channel and the active central connection. No
notify/readback characteristic is required for send-only operation (a connection-
state readback may be added if trivial).

### `main.c` — app bring-up
SoftDevice + ANT + BLE stack init, RTT logging, wiring the three platform modules
to `bridge_core`, and the connection/reconnect policy. No display/battery/UI.

## Concurrency model (top risk)

S340 multiplexes BLE central + BLE peripheral + ANT master over radio timeslots.
The spec commits to a slot budget validated early:

- **Treadmill BLE central:** connection interval chosen to leave room for ANT and
  peripheral advertising while still meeting the iFit keepalive cadence (~1.5s
  poll cycle; writes must land in their phase slots).
- **ANT+ SDM master:** standard SDM channel period (message rate ~4.06 Hz /
  8192-count period). Fixed, defined by the ANT+ device profile.
- **BLE peripheral (control):** low-duty advertising when unconnected; a single
  low-bandwidth connection when the data field is attached.

**Validation step (must pass before feature work):** bring up all three radios
concurrently on the XIAO and confirm the iFit keepalive timing still lands —
i.e. an actual belt-speed change still works with ANT broadcasting and the
control service connected. This is the make-or-break integration check.

## Data field rewrite (send-only)

`garmin_data_field/` changes:

- Add a `Toybox.BluetoothLowEnergy` client: scan → connect to
  `platform_ble_ctrl_svc` → write the target pace read from
  `Activity.Info.currentWorkoutStep` (falling back to `currentSpeed`), on the
  existing ~5s cadence.
- Remove the phone-message path (`workoutStatus` / `targetPace*` app messaging).
- Minimal UI: connection state indicator only. No target-vs-actual readout
  (actual speed is shown by the watch's native ANT sensor / activity data).
- BLE permissions declared in `manifest.xml`.

The Flutter app (`app/`) and `nus_ctrl` become dead code paths for this board but
are left in the repo (retirement is out of scope).

## Testing strategy

- **Host unit tests** (extend `test/host/`): `ant_sdm_encode` page encoding
  (known state → known bytes), plus continued coverage of the reused
  `ftms_parse` / `ifit_parse` / `ctrl_dispatch`.
- **Mock/bench:** reuse `test/mock/mock_treadmill.py` (FTMS) to exercise the BLE
  central path on hardware without the real treadmill. An ANT USB stick (or a
  second Garmin device) confirms the SDM footpod is discoverable and reports the
  right speed.
- **On-watch:** pair the SDM sensor; load the rewritten data field; run a
  structured workout and confirm target pace reaches the belt.
- **Concurrency check:** the make-or-break validation above, on real hardware.

## Risks & gotchas

1. **Radio concurrency budget** — highest risk. Front-loaded as an explicit
   validation gate before feature work.
2. **ANT+ licensing / network key** — ANT+ profiles require the ANT+ network key
   (free registration at thisisant.com). The key is a build-time constant; it is
   **not** committed to the repo (kept in a local/ignored header, like the Garmin
   developer key pattern already used in this project).
3. **XIAO nRF52840 flashing** — the board ships with an Adafruit UF2 bootloader,
   but nRF5 SDK + S340 wants a merged SoftDevice+app hex flashed over **SWD**
   (J-Link/DAPLink on the XIAO's SWD test pads). Bring-up needs a debug probe;
   the alternative (packaging as UF2) is documented as a fallback.
4. **CIQ BLE quirks** — ConnectIQ's `BluetoothLowEnergy` API has per-device
   limits and pairing/bonding quirks; validate a data field can actually connect
   and write to `platform_ble_ctrl_svc` early (spike before full UI work).
5. **iFit keepalive timing under load** — the treadmill silently ignores writes
   outside the poll phase slot; confirm the FSM timing survives S340 event
   latency and concurrent ANT traffic.

## Out of scope

- Retiring/removing the Flutter app and `nus_ctrl` from the repo.
- Display, battery gauge, button UI on the nRF52840.
- Incline control refinements beyond parity with today's behavior.
- Porting the ESP32 boards to any new structure.
