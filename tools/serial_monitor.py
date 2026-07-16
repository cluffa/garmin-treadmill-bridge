#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial"]
# ///
"""
garmin-treadmill-bridge serial monitor + decoder.

Filters out NimBLE GATT-verbose spam and decodes the structured log messages
(JSON state, wkt frames, connection events) into readable output.

Usage:
  ./tools/serial_monitor.py                    # auto-detect port
  ./tools/serial_monitor.py /dev/cu.usbmodem1101
"""

import sys, json, time
import serial, serial.tools.list_ports

# ── port detection ─────────────────────────────────────────────────────────

def find_port():
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "").lower()
        if "usb" in desc or "serial" in desc or "jtag" in desc or "esp" in desc:
            return p.device
    return None

# ── helpers ────────────────────────────────────────────────────────────────

TIMER_NAMES = {0: "OFF", 1: "STOPPED", 2: "PAUSED", 3: "RUN"}

def decode_wkt(parts):
    """Decode a wkt frame log line into human-readable form."""
    d = {}
    for p in parts:
        if "=" in p:
            k, v = p.split("=", 1)
            d[k.strip()] = v.strip()
    ver   = d.get("ver", "?")
    ts    = TIMER_NAMES.get(int(d.get("ts", 0)), d.get("ts", "?"))
    flags = d.get("fl", "0x00")
    has_step = (int(flags, 16) & 0x01) != 0
    intensity = int(d.get("int", 0))
    target_type = int(d.get("tt", 0))
    lo = int(d.get("lo", 0))
    hi = int(d.get("hi", 0))
    dur_type = int(d.get("dur", 0))
    dur_val  = int(d.get("dv", 0))
    rep = int(d.get("rep", 0))

    ttype_name = "SPEED" if target_type == 0 else f"type={target_type}" if target_type != 255 else "none"
    intensity_name = "REST" if intensity == 1 else "ACTIVE" if intensity == 2 else "WARMUP" if intensity == 3 else "COOLDOWN" if intensity == 4 else f"{intensity}" if intensity != 255 else "-"

    if has_step:
        kmh = 0.0
        if target_type == 0 and (lo > 0 or hi > 0):
            mmps = (lo + hi) // 2 if (lo and hi) else (lo or hi)
            kmh = mmps * 0.0036
        return (f"  📤 wkt frame: timer={ts}, intensity={intensity_name}, "
                f"target={ttype_name}, speed_lo={lo}_hi={hi}_mm/s (~{kmh:.1f} km/h), "
                f"dur_type={dur_type}, dur_val={dur_val}, rep={rep}")
    else:
        return f"  📤 wkt frame: timer={ts}, no_step"

def decode_state(line):
    """Decode a JSON state event."""
    try:
        obj = json.loads(line)
        s = obj.get("speed", 0)
        d = obj.get("distance", 0)
        i = obj.get("incline", 0)
        return f"  🏃 treadmill: {s:.1f} km/h  |  {d:.1f} m  |  {i:.1f}% incline"
    except (json.JSONDecodeError, KeyError):
        return None

# ── main ───────────────────────────────────────────────────────────────────

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    if not port:
        print("No serial port found. Pass one as an argument.")
        sys.exit(1)

    print(f"Opening {port}…", flush=True)
    ser = serial.Serial(port, 115200, timeout=0.5)
    time.sleep(0.3)
    ser.reset_input_buffer()

    print("Monitoring — Ctrl-C to stop.\n", flush=True)

    while True:
        try:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue

            # ── skip garbled / partial nimble lines ──
            if len(line) < 8 or (len(line) < 50 and any(ch.isdigit() for ch in line[:8]) and not line.startswith("{")):
                continue

            lower = line.lower()

            # ── skip nimble gatt verbose ──
            if ("nimble:" in lower and ("gatt procedure" in lower
                                        or "att_handle" in lower
                                        or "peer_addr" in lower
                                        or "scan_itvl" in lower
                                        or "reattempt" in lower
                                        or "advertise" in lower
                                        or "disc_mode" in lower
                                        or "adv_channel" in lower)):
                continue

            # ── decode known structured lines ──

            # wkt frame
            if "wkt frame:" in lower:
                print(decode_wkt(line.split()))
                continue

            # JSON state
            if '{"event":"state"' in line:
                decoded = decode_state(line)
                if decoded:
                    print(decoded, flush=True)
                continue

            # JSON commands
            if line.startswith("{"):
                try:
                    obj = json.loads(line)
                    cmd = obj.get("cmd", "")
                    if cmd == "status":
                        print(f"  🔗 status: connected={obj.get('connected')}, "
                              f"name={obj.get('name', '?')}")
                    elif cmd == "list":
                        devs = obj.get("devices", [])
                        print(f"  📋 list: {len(devs)} devices")
                    else:
                        print(f"  ⬡ json: {line}")
                    continue
                except json.JSONDecodeError:
                    pass

            # Connection events (ctrl_svc / garmin_rsc)
            if "ctrl_svc:" in lower and "watch connected" in lower:
                print(f"  ⌚ watch BLE connected")
                continue
            if "ctrl_svc:" in lower and "watch disconnected" in lower:
                reason = ""
                if "reason=" in line:
                    reason = line.split("reason=")[1].split()[0].rstrip()
                print(f"  ⌚ watch BLE disconnected (reason={reason})" if reason else f"  ⌚ watch BLE disconnected")
                continue
            if "rsc tracking" in lower:
                print(f"  📡 RSC sensor tracking")
                continue

            # Treadmill connection
            if "machine: connected" in lower:
                name = ""
                if "(" in line:
                    name = line.split("(")[1].split(")")[0]
                print(f"  🏭 treadmill connected ({name})" if name else "  🏭 treadmill connected")
                continue
            if "machine: disconnected" in lower:
                print(f"  🏭 treadmill disconnected")
                continue
            if "machine: reconnecting" in lower:
                print(f"  🔄 treadmill reconnecting...")
                continue

            # Advertising
            if "CTRL+RSC advertising" in line:
                print(f"  📶 BLE advertising started")
                continue

            # Boot
            if "boot complete" in lower:
                print(f"  ✅ boot complete")
                continue

            # Warnings / errors
            if "W (" in line or "E (" in line:
                print(f"  ⚠️  {line}")
                continue
            if "wkt write from conn" in lower:
                print(f"  ❌ {line}")
                continue

            # iFit / poll
            if "starting poll" in lower:
                print(f"  🔁 iFit poll started")
                continue

            # ── pass through anything else ──
            if line:
                print(f"  · {line}", flush=True)

        except serial.SerialException:
            print("\nSerial disconnected.", flush=True)
            break
        except KeyboardInterrupt:
            print("\nStopped.", flush=True)
            break

if __name__ == "__main__":
    main()
