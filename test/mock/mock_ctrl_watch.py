#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["bleak"]
# ///
"""Mock Garmin ctrl app / data field: drive the bridge's A6ED control service.

Connects to the bridge (ESP32 or nRF52840 — same GATT contract), subscribes
to the response characteristic, decodes the compact 'D'/'E'/'S' frames, and
forwards stdin lines (STATUS, LIST, SCAN, CONNECT <n>, SPEED <kmh>,
INCLINE <pct>, STOP) to the control characteristic.
"""
import asyncio, sys
from bleak import BleakScanner, BleakClient

CTRL_SVC = "a6ed0001-d344-460a-8075-b9e8ec90d71b"
CTRL_CHR = "a6ed0002-d344-460a-8075-b9e8ec90d71b"
RSP_CHR  = "a6ed0003-d344-460a-8075-b9e8ec90d71b"

FLAG_CONNECTED = 0x01
FLAG_SAVED     = 0x02


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
        print("subscribed — commands: STATUS LIST SCAN CONNECT <n> "
              "SPEED <kmh> INCLINE <pct> STOP (Ctrl-D quits)")

        loop = asyncio.get_event_loop()
        while True:
            line = await loop.run_in_executor(None, sys.stdin.readline)
            if not line:            # EOF
                break
            line = line.strip()
            if not line:
                continue
            await c.write_gatt_char(CTRL_CHR, line.upper().encode(),
                                    response=False)
            print("  ->", line.upper())
            await asyncio.sleep(0.3)   # let replies print before the prompt


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
