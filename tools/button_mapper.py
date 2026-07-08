"""
button_mapper.py — interactive controller button -> telemetry-bit logger.

The firmware's `km.telem` stream reports physical buttons as a hex bitmask
(`b=2000`). This tool prompts you to press each button in turn, watches which
bit lights up, and writes the mapping to tools/button_map.json — which the
GUI's Monitor tab then uses to show button NAMES instead of raw hex
(e.g. LB=0x1000, RB=0x2000).

Usage:  python tools/button_mapper.py [COM5]
        (port defaults to the CH343 auto-detect, then config.json)

Press each button when prompted, or Enter to skip one. Ctrl+C to abort.
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "accessibility"))
from makcu_access import Makcu, load_config

import serial.tools.list_ports

CH343_VID = 0x1A86
BUTTONS_TO_MAP = ["A", "B", "X", "Y", "LB", "RB", "View", "Menu",
                  "LS-click", "RS-click", "DPad-Up", "DPad-Down",
                  "DPad-Left", "DPad-Right", "Xbox/Guide"]
SETTLE_S = 0.15      # let the stream settle after the prompt
CAPTURE_S = 4.0      # how long to wait for a press


def find_port():
    if len(sys.argv) > 1:
        return sys.argv[1]
    for p in serial.tools.list_ports.comports():
        if p.vid == CH343_VID:
            return p.device
    return load_config()["port"]


def read_mask(mk, seconds):
    """Return the OR of all b-bits seen within `seconds` (0 if none).
    Reads the serial stream directly with a hard deadline so a silent
    stream (controller unplugged) can't hang the prompt."""
    mask = 0
    buf = b""
    end = time.time() + seconds
    while time.time() < end:
        buf += mk.ser.read(256)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            s = line.decode("ascii", "replace").strip()
            if not s.startswith("KMS "):
                continue
            for tok in s[4:].split():
                k, _, v = tok.partition("=")
                if k == "b":
                    try:
                        mask |= int(v, 16)
                    except ValueError:
                        pass
    return mask


def main():
    port = find_port()
    print(f"connecting on {port} …")
    mk = Makcu(port)
    v = mk.version().strip()
    if not v.startswith("kmbox"):
        print(f"unexpected handshake reply: {v!r} — wrong port?")
        return 1
    print("link:", v.splitlines()[0])
    mk.telem(True)
    time.sleep(SETTLE_S)

    print("\nRelease all buttons …")
    time.sleep(1.0)
    baseline = read_mask(mk, 1.0)
    if baseline:
        print(f"warning: bits already set at rest: {baseline:#06x} "
              "(stuck button or drift?) — they will be ignored.")

    mapping = {}
    print("\nPress and HOLD each button when prompted "
          "(Enter to skip, Ctrl+C to abort):\n")
    try:
        for name in BUTTONS_TO_MAP:
            input(f"  ready for {name!r}? press Enter, then hold the button … ")
            mask = read_mask(mk, CAPTURE_S) & ~baseline
            if mask:
                mapping[name] = f"{mask:#06x}"
                print(f"    {name} = {mask:#06x}")
            else:
                print(f"    no bit seen for {name} — skipped "
                      "(triggers/sticks are analog, not in the b mask)")
    except KeyboardInterrupt:
        print("\naborted — writing what we have.")
    finally:
        mk.telem(False)
        mk.ser.close()

    if not mapping:
        print("nothing captured; not writing a file.")
        return 1
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "button_map.json")
    with open(out, "w") as fh:
        json.dump(mapping, fh, indent=2)
    print(f"\nwrote {len(mapping)} buttons -> {out}")
    print("The GUI Monitor tab will now show button names.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
