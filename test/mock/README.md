# Mock BLE devices for testing garmin-ftms-sync

Python harnesses to exercise the firmware's two BLE roles from a Mac (or any
host with BLE), without real fitness equipment.

## sensor_logger.py — log all RSC + FTMS sensors at once

A BLE central that scans for every device advertising RSC (0x1814) or FTMS
(0x1826), connects to all of them concurrently, subscribes to their measurement
characteristics, and prints + CSV-logs timestamped, parsed notifications.
Parses RSC, FTMS Treadmill, and Indoor Bike data; raw-hex for others. Good for
comparing the bridge's RSC output against a real treadmill side by side.

```sh
./venv/bin/python sensor_logger.py        # Ctrl-C to stop; writes sensor_log_*.csv
```

Note: many fitness machines accept only one BLE connection, so if the bridge is
already connected to the treadmill you can't also read it directly here.

## Setup

The scripts are self-contained [uv](https://docs.astral.sh/uv/) scripts — each
declares its own dependencies inline (PEP 723), so just run them directly and
uv fetches the packages into an ephemeral env:

```sh
./sensor_logger.py        # or ./mock_watch.py, ./mock_treadmill.py
```

(Requires `uv` installed. No venv needed.) If you'd rather use a plain venv:

```sh
python3 -m venv venv && ./venv/bin/pip install bleak bless
./venv/bin/python sensor_logger.py
```

`bleak` (central) works well on macOS. `bless` (peripheral) uses CoreBluetooth
via pyobjc; the host's terminal app needs Bluetooth permission.

## mock_watch.py — stands in for the Garmin watch

A BLE **central** that scans for `garmin-ftms-sync`, connects, reads the RSC
Feature characteristic (expects `0x0006`), subscribes to RSC Measurement
notifications, and decodes them (speed, cadence, distance, running flag).

```sh
./venv/bin/python mock_watch.py
```

Verified working: with the bridge flashed and an FTMS treadmill connected, this
prints live RSC notifications end-to-end. Use it to confirm the peripheral side
(advertising, GATT service, feature bits, subscribe, notify).

## mock_ctrl_watch.py — stands in for the CIQ data field / ctrl app

A BLE **central** that scans for the `A6ED0001-…` control service (ESP32 or
nRF52840 bridge — same contract), connects, subscribes to the response
characteristic, and decodes the compact `'D'/'E'/'S'` frames. Type commands on
stdin (`STATUS`, `LIST`, `SCAN`, `CONNECT <n>`, `SPEED <kmh>`, `INCLINE <pct>`,
`STOP`); they're written to the control characteristic uppercased.

```sh
./mock_ctrl_watch.py
```

Use it to exercise the watch control path without flashing the Garmin apps:
the `'S'` greeting on subscribe, `LIST` replies, unsolicited `'S'` on
treadmill link changes, and speed/incline dispatch.

## mock_treadmill.py — stands in for an FTMS treadmill

A BLE **peripheral** advertising the Fitness Machine Service (0x1826) with a
Treadmill Data characteristic (0x2ACD) that notifies a simulated run (speed
ramps to 12 km/h, distance accumulates, incline 1.5%).

```sh
./venv/bin/python -u mock_treadmill.py
```

**Known limitation:** on macOS/CoreBluetooth (bless), the 0x1826 service is not
reliably discovered by NimBLE's by-UUID service discovery — the bridge connects
but logs "FTMS service (0x1826) not found". The firmware's central path is
instead validated against a real FTMS device (see the session notes); this mock
is best-effort and may need a different BLE peripheral host (e.g. Linux/BlueZ
with `bluezero`, or an nRF/ESP devkit running an FTMS peripheral sketch) to be
fully usable. The dynamic value path is covered by the host unit tests
(`test/host`).

## Capturing firmware logs

`idf.py monitor` is interactive; for scripted capture use pyserial directly,
e.g. read `/dev/cu.usbserial-*` at 115200 (pulse DTR/RTS to reset).
