"""
inject_mapper.py — map what each km.* button command ACTUALLY presses.

The firmware's injected button bits may not line up with what the console
reads (e.g. km.lb landing on DPad-Down). This tool fires each km command
one at a time — you watch a game/controller-test screen and type in what
really happened. Results go to tools/inject_map.json.

Usage:  python tools/inject_mapper.py [COM5]
        Get into an offline game (or the console's controller test screen)
        where button presses are visible, then follow the prompts.
        Wiggle a stick during each hold — GIP pads only emit reports on
        change and injection rides on reports.

Each command is held HOLD_S seconds. Type what it pressed (e.g. "dpad-down",
"A", "nothing"), or just Enter for "nothing seen". Ctrl+C aborts (partial
results are still written).
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
HOLD_S = 2.0

# every button-ish km command the firmware implements, with what it SHOULD be
CMDS = [
    ("km.btnA",   "A"),
    ("km.btnB",   "B"),
    ("km.btnX",   "X"),
    ("km.btnY",   "Y"),
    ("km.lb",     "LB"),
    ("km.rb",     "RB"),
    ("km.left",   "RT (fire)"),
    ("km.right",  "LT (ADS)"),
    ("km.middle", "X alias"),
]


def find_port():
    if len(sys.argv) > 1:
        return sys.argv[1]
    for p in serial.tools.list_ports.comports():
        if p.vid == CH343_VID:
            return p.device
    return load_config()["port"]


def main():
    port = find_port()
    print(f"connecting on {port} …")
    mk = Makcu(port)
    v = mk.version().strip()
    if not v.startswith("kmbox"):
        print(f"warning: odd handshake reply {v!r} — continuing anyway "
              "(commands may still work).")
    else:
        print("link:", v.splitlines()[0])

    print(__doc__.split("Usage:")[0])
    input("Ready? Get the game visible, hand on the stick, then press Enter … ")

    results = {}
    try:
        for cmd, expect in CMDS:
            input(f"\nnext: {cmd}  (should be {expect}) — Enter to fire … ")
            print(f"  holding {cmd} for {HOLD_S:g}s — WATCH (and wiggle) …")
            mk._send(f"{cmd}(1)")
            time.sleep(HOLD_S)
            mk._send(f"{cmd}(0)")
            seen = input("  what did it press? (Enter = nothing seen) ").strip()
            results[cmd] = {"expected": expect, "actual": seen or "nothing"}
    except KeyboardInterrupt:
        print("\naborted — writing what we have.")
    finally:
        # belt and braces: release everything this tool can have touched
        for cmd, _ in CMDS:
            try:
                mk._send(f"{cmd}(0)")
            except Exception:
                pass
        mk.ser.close()

    if not results:
        print("nothing captured; not writing a file.")
        return 1
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "inject_map.json")
    with open(out, "w") as fh:
        json.dump(results, fh, indent=2)

    print(f"\nwrote {out}\n")
    print(f"{'command':<12} {'expected':<12} actual")
    mismatches = 0
    for cmd, r in results.items():
        mark = ""
        if r["actual"].lower() not in (r["expected"].lower(), "nothing"):
            mark = "  <-- MISMATCH"
            mismatches += 1
        print(f"{cmd:<12} {r['expected']:<12} {r['actual']}{mark}")
    if mismatches:
        print(f"\n{mismatches} mismatch(es) — this table is exactly what's "
              "needed to fix the button map in km_inject.c.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
