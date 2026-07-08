#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["bleak"]
# ///
"""Mock Garmin ctrl app / data field: drive the bridge's A6ED control service.

Connects to the bridge (ESP32 or nRF52840 — same GATT contract), subscribes
to the response characteristic, decodes the compact 'D'/'E'/'S' frames, and
forwards stdin lines to the bridge.

Two command families:
  * ctrl grammar (uppercase, to CTRL_CHR): STATUS, LIST, SCAN, CONNECT <n>,
    SPEED <kmh>, INCLINE <pct>, STOP — the picker-app path.
  * workout telemetry (to WKT_CHR, decoded by workout_ctrl.c) — the data-field
    path this exercises:
      TARGET <kmh>      running, speed target <kmh>
      RANGE <lo> <hi>   running, speed target range (km/h)
      REST              running, OPEN target (bridge holds last speed)
      FREERUN           running, no workout step
      PAUSE             timer paused (bridge stops the belt)
      RESUME <kmh>      timer running again, speed target <kmh>
      END               timer stopped (bridge stops the belt)
"""
import asyncio, sys
from bleak import BleakScanner, BleakClient

CTRL_SVC = "a6ed0001-d344-460a-8075-b9e8ec90d71b"
CTRL_CHR = "a6ed0002-d344-460a-8075-b9e8ec90d71b"
RSP_CHR  = "a6ed0003-d344-460a-8075-b9e8ec90d71b"
WKT_CHR  = "a6ed0004-d344-460a-8075-b9e8ec90d71b"

FLAG_CONNECTED = 0x01
FLAG_SAVED     = 0x02

# Activity enum values mirrored from the watch (see workout_ctrl.h).
TIMER_STOPPED, TIMER_PAUSED, TIMER_ON = 1, 2, 3
TGT_SPEED, TGT_OPEN = 0, 2


def wkt_frame(timer, has_step, target=0xFF, low_mmps=0, high_mmps=0,
              intensity=0xFF, dur_type=0xFF, dur_val=0, rep=0) -> bytes:
    """Pack a 15-byte workout-telemetry frame (matches workout_ctrl.h)."""
    f = bytearray(15)
    f[0] = 1                       # version
    f[1] = timer & 0xFF
    f[2] = 0x01 if has_step else 0x00
    f[3] = intensity & 0xFF
    f[4] = target & 0xFF
    f[5:7] = int(low_mmps).to_bytes(2, "little")
    f[7:9] = int(high_mmps).to_bytes(2, "little")
    f[9] = dur_type & 0xFF
    f[10:14] = int(dur_val).to_bytes(4, "little")
    f[14] = rep & 0xFF
    return bytes(f)


def kmh_to_mmps(kmh: float) -> int:
    return int(round(kmh / 3.6 * 1000.0))


def build_telemetry(line: str):
    """Return a 15-byte frame for a telemetry command, or None if not one."""
    parts = line.split()
    cmd = parts[0].upper()
    try:
        if cmd == "TARGET":
            m = kmh_to_mmps(float(parts[1]))
            return wkt_frame(TIMER_ON, True, TGT_SPEED, m, m)
        if cmd == "RANGE":
            lo, hi = kmh_to_mmps(float(parts[1])), kmh_to_mmps(float(parts[2]))
            return wkt_frame(TIMER_ON, True, TGT_SPEED, lo, hi)
        if cmd == "REST":
            return wkt_frame(TIMER_ON, True, TGT_OPEN, 0, 0)
        if cmd == "FREERUN":
            return wkt_frame(TIMER_ON, False)
        if cmd == "PAUSE":
            return wkt_frame(TIMER_PAUSED, False)
        if cmd == "RESUME":
            m = kmh_to_mmps(float(parts[1]))
            return wkt_frame(TIMER_ON, True, TGT_SPEED, m, m)
        if cmd == "END":
            return wkt_frame(TIMER_STOPPED, False)
    except (IndexError, ValueError):
        print("  !! bad args for", cmd)
        return b""   # signal "handled, but nothing to send"
    return None


def decode(frame: bytes) -> str:
    if not frame:
        return "<empty>"
    tag = chr(frame[0])
    if tag == "D" and len(frame) >= 5:
        idx = frame[1]
        rssi = int.from_bytes(frame[2:3], "little", signed=True)
        proto = "iFit" if frame[3] else "FTMS"
        flags = frame[4]
        name = frame[5:].split(b"\0")[0].decode(errors="replace")
        marks = ("*" if flags & FLAG_CONNECTED else "") + \
                ("+" if flags & FLAG_SAVED else "")
        return f"D idx={idx} rssi={rssi} proto={proto} name={name!r} {marks}"
    if tag == "E" and len(frame) >= 2:
        return f"E count={frame[1]}"
    if tag == "S" and len(frame) >= 3:
        proto = "iFit" if frame[2] else "FTMS"
        name = frame[3:].split(b"\0")[0].decode(errors="replace")
        state = "connected" if frame[1] else "disconnected"
        return f"S {state} proto={proto} name={name!r}"
    return f"? {frame.hex()}"


async def main():
    print("scanning for control service", CTRL_SVC, "...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: CTRL_SVC in (ad.service_uuids or []), timeout=15)
    if not dev:
        print("DEVICE NOT FOUND")
        return
    print("found:", dev.address, dev.name)
    async with BleakClient(dev) as c:
        print("connected:", c.is_connected)

        def on_rsp(_, data: bytearray):
            print("  <-", decode(bytes(data)))

        await c.start_notify(RSP_CHR, on_rsp)
        print("subscribed — ctrl: STATUS LIST SCAN CONNECT <n> SPEED <kmh> "
              "INCLINE <pct> STOP")
        print("            telemetry: TARGET <kmh> | RANGE <lo> <hi> | REST | "
              "FREERUN | PAUSE | RESUME <kmh> | END   (Ctrl-D quits)")

        loop = asyncio.get_event_loop()
        while True:
            line = await loop.run_in_executor(None, sys.stdin.readline)
            if not line:            # EOF
                break
            line = line.strip()
            if not line:
                continue
            frame = build_telemetry(line)
            if frame is not None:
                if frame:           # a real frame (not a parse error)
                    await c.write_gatt_char(WKT_CHR, frame, response=False)
                    print("  => wkt", frame.hex())
            else:
                await c.write_gatt_char(CTRL_CHR, line.upper().encode(),
                                        response=False)
                print("  ->", line.upper())
            await asyncio.sleep(0.3)   # let replies print before the prompt


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
