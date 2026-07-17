"""
ps4_check.py — one-shot diagnostic for "PS4 pad passes through but km.* does nothing".

What it does, automatically:
  1. finds the KM serial port (middle USB / CH343) and handshakes
  2. turns on km.telem and captures 4 s of KMS lines
  3. tells you in plain words whether the firmware's DS4/DS5 injection
     branch is matching this controller's reports (b0/len check)
  4. runs a 3 s km.move burst so you can watch the right stick in a
     gamepad tester (e.g. https://hardwaretester.com/gamepad or the game)

Usage:  python tools/ps4_check.py [COMx]
"""

import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "accessibility"))
from makcu_access import Makcu, load_config

import serial.tools.list_ports

CH343_VID = 0x1A86
CAPTURE_S = 4.0
MOVE_S = 3.0


def find_port():
    if len(sys.argv) > 1:
        return sys.argv[1]
    for p in serial.tools.list_ports.comports():
        if p.vid == CH343_VID:
            return p.device
    return load_config()["port"]


def capture(mk, seconds):
    """Collect KMS lines for `seconds`; return list of dicts."""
    rows = []
    buf = b""
    end = time.time() + seconds
    while time.time() < end:
        buf += mk.ser.read(256)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            s = line.decode("ascii", "replace").strip()
            if not s.startswith("KMS "):
                continue
            row = {}
            for tok in s[4:].split():
                k, _, v = tok.partition("=")
                try:
                    row[k] = int(v, 16) if k in ("b", "ep", "b0") else int(v)
                except ValueError:
                    pass
            if row:
                rows.append(row)
    return rows


def main():
    port = find_port()
    print(f"connecting on {port} ...")
    mk = Makcu(port)
    v = mk.version().strip()
    if not v.startswith("kmbox"):
        print(f"unexpected handshake reply: {v!r} — wrong port?")
        return 1
    print("link ok:", v.splitlines()[0])

    print(f"\ncapturing telemetry for {CAPTURE_S:.0f} s — WIGGLE THE RIGHT STICK now ...")
    mk.telem(True)
    rows = capture(mk, CAPTURE_S)
    mk.telem(False)

    if not rows:
        print("\nRESULT: no KMS telemetry lines at all.")
        print("  -> Left MCU not running a telemetry-capable build, or wrong port.")
        return 1

    first_n, last_n = rows[0].get("n", 0), rows[-1].get("n", 0)
    ep = rows[-1].get("ep", -1)
    b0 = rows[-1].get("b0", -1)
    ln = rows[-1].get("len", -1)
    rx_moved = max(abs(r.get("rx", 0)) for r in rows) > 3000 or \
               max(abs(r.get("ry", 0)) for r in rows) > 3000

    print(f"\nlast line:  n={last_n} ep={ep:02x} b0={b0:02x} len={ln}")
    print(f"km_apply calls during capture: {last_n - first_n}")

    if last_n == first_n:
        print("\nRESULT: km_apply is NOT being called.")
        print("  -> reports aren't reaching the injection hook; Right/Left forwarding issue.")
        return 1

    if b0 == 0x01 and ln >= 64:
        print("\nRESULT: DS4/DS5 branch MATCHES (b0=01, len>=64).")
        if rx_moved:
            print("  right-stick telemetry moved — stick byte layout is correct.")
        else:
            print("  right-stick telemetry did NOT move — layout may be off (did you wiggle it?).")
        print("  km.move should inject; km.click/buttons are known-wrong for DS4 (DS5 layout).")
    else:
        print(f"\nRESULT: report does NOT match the DS4/DS5 branch (need b0=01 len>=64).")
        print("  -> firmware needs a new adapter for this report format. Send me this output.")

    input(f"\nENTER to run a {MOVE_S:.0f} s km.move burst "
          "(watch right stick in a gamepad tester) ... ")
    end = time.time() + MOVE_S
    while time.time() < end:
        mk.move(15, 0)
        time.sleep(0.008)
    print("burst done. Did the right stick move right? That's the answer to report.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\naborted.")
