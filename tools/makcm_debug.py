"""
makcm_debug.py — guided "no throughput" debugger for the MAKCM passthrough.

Connects to the CH343 command port (middle USB), enables the firmware's
runtime diagnostic stream (km.debug(1), Left build >= Jul 2026-07-10),
walks you through replugging the controller, then analyzes the captured
[R]/[L] log lines and telemetry to say WHERE the pipeline breaks:

    controller -> Right MCU (USB host) -> IPC -> Left MCU -> console

Usage:
    pip install pyserial
    python tools/makcm_debug.py [--port COM7] [--seconds 20] [--keep-debug]

Wiring while debugging: PC -> USB2 (middle). USB1 must also be plugged in
(console or PC — it powers the Left MCU). Controller -> USB3.

The full raw capture is saved to makcm_debug_<timestamp>.log next to the
report so it can be attached to a GitHub issue.
"""

import argparse
import re
import sys
import time
from datetime import datetime

import serial
import serial.tools.list_ports

CH343_VID = 0x1A86
KM_BAUD = 4_000_000


def find_ch343():
    for p in serial.tools.list_ports.comports():
        if p.vid == CH343_VID:
            return p.device
    return None


def read_for(ser, seconds, sink):
    """Read raw bytes for `seconds`, append decoded lines to sink, return them."""
    deadline = time.monotonic() + seconds
    buf = b""
    new_lines = []
    while time.monotonic() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("ascii", "replace").rstrip("\r")
                sink.append(text)
                new_lines.append(text)
        else:
            time.sleep(0.02)
    if buf:
        text = buf.decode("ascii", "replace")
        sink.append(text)
        new_lines.append(text)
    return new_lines


def send(ser, cmd):
    ser.write((cmd + "\n").encode("ascii"))


def analyze(lines):
    """Reduce the capture to the pipeline facts we care about."""
    facts = {
        "new_dev": False,
        "dev_ids": None,          # (vid, pid)
        "claims": [],             # (ifnum, err, cls)
        "in_eps": [],             # (ep, mps, err)
        "out_eps": [],            # ep addresses seen (last one is GIP target)
        "gip_sends": [],          # (cmd, ok)
        "in_done_announce": 0,    # b0=02 completions
        "in_done_input": 0,       # b0=20 completions
        "in_done_other": [],      # (b0, status) others
        "desc_fail": False,
        "device_gone": 0,
        "kms_n": [],              # telemetry n= counter samples
        "r_lines": 0,
    }
    for ln in lines:
        if ln.startswith("KMS "):
            m = re.search(r"\bn=(\d+)", ln)
            if m:
                facts["kms_n"].append(int(m.group(1)))
            continue
        if not ln.startswith("[R] "):
            if "DEVICE_GONE" in ln:
                facts["device_gone"] += 1
            continue
        facts["r_lines"] += 1
        body = ln[4:]
        if body.startswith("NEW_DEV"):
            facts["new_dev"] = True
        m = re.match(r"dev VID=([0-9a-fA-F]{4}) PID=([0-9a-fA-F]{4})", body)
        if m:
            facts["dev_ids"] = (m.group(1), m.group(2))
        m = re.match(r"claim if=(\d+) err=0x([0-9a-fA-F]+) cls=(\S+)", body)
        if m:
            facts["claims"].append((int(m.group(1)), int(m.group(2), 16), m.group(3)))
        m = re.match(r"IN open ep=([0-9a-fA-F]{2}) mps=(\d+) submit_err=0x([0-9a-fA-F]+)", body)
        if m:
            facts["in_eps"].append((m.group(1), int(m.group(2)), int(m.group(3), 16)))
        m = re.match(r"OUT ep=([0-9a-fA-F]{2})", body)
        if m:
            facts["out_eps"].append(m.group(1))
        m = re.match(r"GIP send cmd=([0-9a-fA-F]{2}) seq=\d+ ok=(\d)", body)
        if m:
            facts["gip_sends"].append((m.group(1), m.group(2) == "1"))
        m = re.match(r"IN done ep=([0-9a-fA-F]{2}) status=(-?\d+) nbytes=(\d+) b0=([0-9a-fA-F]{2})", body)
        if m:
            b0 = m.group(4).lower()
            if b0 == "20":
                facts["in_done_input"] += 1
            elif b0 == "02":
                facts["in_done_announce"] += 1
            else:
                facts["in_done_other"].append((b0, int(m.group(2))))
        if "descriptor relay failed" in body or "fetch_and_relay_descriptors=0" in body:
            facts["desc_fail"] = True
        if body.startswith("DEV_GONE"):
            facts["device_gone"] += 1
    return facts


def verdict(facts, debug_ack, telem_seconds):
    """Return (headline, details[]) — where the pipeline breaks."""
    d = []
    kms = facts["kms_n"]
    kms_delta = (kms[-1] - kms[0]) if len(kms) >= 2 else 0

    if not debug_ack and facts["r_lines"] == 0:
        return ("Left firmware predates km.debug() — no diagnostics available.",
                ["Reflash the LEFT MCU with the current repo build (or a log build:",
                 "  pio run -d firmware/MAKCM_ESP32s3_Pass_Left_IDF -e LEFT_IDF),",
                 "then rerun this tool."])

    if not facts["new_dev"]:
        if facts["r_lines"] == 0:
            return ("No [R] lines at all — the RIGHT MCU never spoke over IPC.",
                    ["Either the Right MCU is not running this repo's firmware, or it",
                     "was not power-cycled after flashing (unplug ALL cables, wait 5 s,",
                     "replug), or the IPC link is down.",
                     "Fix: reflash MERGED_right.bin per FLASHING.md, full power-cycle."])
        return ("Right MCU is alive but never saw the controller enumerate (no NEW_DEV).",
                ["The controller did not show up on USB3 during the capture window.",
                 "Check: controller plugged into USB3 (right port)? Cable carries data",
                 "(charge-only cables don't)? Controller in wired/Xbox mode, not",
                 "wireless/dongle/Bluetooth mode? Try another cable/port."])

    if facts["desc_fail"]:
        return ("Controller enumerated but descriptor relay FAILED.",
                ["The Right MCU could not read/relay the USB descriptors — flaky cable,",
                 "power, or an enumeration quirk of this controller. Save the .log file",
                 "and open a GitHub issue with it."])

    if facts["in_done_input"] == 0:
        det = []
        if facts["dev_ids"]:
            det.append(f"Controller identified: VID={facts['dev_ids'][0]} PID={facts['dev_ids'][1]}.")
        if facts["in_done_announce"] > 0:
            det += [f"Controller sent {facts['in_done_announce']} GIP announce packets but",
                    "never produced an input report (b0=0x20) — the GIP init handshake",
                    "is not being accepted."]
        else:
            det += ["No input reports and no announce packets — the controller opened",
                    "endpoints but stayed silent."]
        if len(set(facts["out_eps"])) > 1:
            det += [f"NOTE: multiple OUT endpoints seen ({', '.join(sorted(set(facts['out_eps'])))});",
                    "GIP init is sent to the LAST one learned — on multi-interface",
                    "controllers that can be the wrong endpoint. Include the .log in a",
                    "GitHub issue; this is diagnosable from it."]
        fails = [c for c, ok in facts["gip_sends"] if not ok]
        if fails:
            det.append(f"GIP sends that FAILED to submit: cmds {', '.join(fails)}.")
        det.append("Reflash the Right MCU with the current repo build and power-cycle;")
        det.append("if it persists, attach the .log to a GitHub issue.")
        return ("Break is on the RIGHT side: controller present, no input reports.", det)

    # Input reports flow. Is telemetry moving too (Right -> IPC -> Left OK)?
    if kms_delta == 0 and telem_seconds > 0:
        return ("Controller streams input, but the LEFT MCU shows no telemetry.",
                ["Right MCU receives input reports fine, telemetry counter never moved —",
                 "the IPC EP_IN path or the Left firmware is not processing them.",
                 "Reflash the LEFT MCU with the current repo build and power-cycle."])

    rate = kms_delta / telem_seconds if telem_seconds else 0
    return ("Board pipeline is HEALTHY — controller input reaches the Left MCU"
            f" (~{rate:.0f} reports/s). The break is on the CONSOLE side.",
            ["The console only inspects the device when USB1 attaches. Fix order:",
             "1) leave everything else connected, 2) unplug USB1, wait 5 s,",
             "3) replug USB1 LAST. Also verify topology: PC->USB2, console->USB1,",
             "controller->USB3 (left/right swapped = silent failure)."])


def main():
    ap = argparse.ArgumentParser(description="MAKCM no-throughput debugger")
    ap.add_argument("--port", help="CH343 command port (default: auto-detect)")
    ap.add_argument("--seconds", type=int, default=20,
                    help="capture window after replug prompt (default 20)")
    ap.add_argument("--keep-debug", action="store_true",
                    help="leave km.debug(1) enabled on exit")
    args = ap.parse_args()

    port = args.port or find_ch343()
    if not port:
        print("FAIL: no CH343 command port found (VID 1A86).")
        print("Plug the MIDDLE cable (USB2) into this PC; install the WCH CH343 driver if needed.")
        sys.exit(1)
    print(f"Command port: {port}")

    ser = serial.Serial(port, KM_BAUD, timeout=0.2)
    capture = []
    try:
        time.sleep(0.3)
        ser.reset_input_buffer()

        send(ser, "km.version()")
        hs = " ".join(read_for(ser, 1.0, capture))
        if "kmbox" not in hs:
            print("FAIL: no km.version() reply — Left MCU not answering.")
            print("USB1 must also be plugged in (it powers the Left MCU), and the Left")
            print("MCU must run this repo's firmware. See FLASHING.md.")
            sys.exit(1)
        print(f"Handshake OK: {hs.strip().splitlines()[0] if hs.strip() else hs}")

        send(ser, "km.debug(1)")
        ack = " ".join(read_for(ser, 0.8, capture))
        debug_ack = "km.debug" in ack
        if not debug_ack:
            print("NOTE: no km.debug ack — Left firmware may predate the runtime log")
            print("toggle (pre 2026-07-10). Continuing; a log build will still work.")
        send(ser, "km.telem(1)")
        read_for(ser, 0.3, capture)

        print()
        print(f"Now UNPLUG the controller from USB3, wait 3 s, REPLUG it, then wiggle")
        print(f"the sticks and press buttons. Capturing for {args.seconds} s ...")
        t0 = time.monotonic()
        read_for(ser, args.seconds, capture)
        telem_seconds = time.monotonic() - t0

        facts = analyze(capture)
        head, details = verdict(facts, debug_ack, telem_seconds)

        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        logfile = f"makcm_debug_{stamp}.log"
        with open(logfile, "w", encoding="utf-8") as f:
            f.write("\n".join(capture) + "\n")

        print()
        print("=" * 70)
        print("VERDICT:", head)
        for line in details:
            print(" ", line)
        print("=" * 70)
        if facts["dev_ids"]:
            print(f"Controller: VID={facts['dev_ids'][0]} PID={facts['dev_ids'][1]}")
        print(f"Interfaces claimed: {len(facts['claims'])}   "
              f"IN eps: {len(facts['in_eps'])}   OUT eps: {sorted(set(facts['out_eps']))}")
        print(f"GIP announces: {facts['in_done_announce']}   "
              f"input reports seen (first 20 logged): {facts['in_done_input']}")
        kms = facts["kms_n"]
        if len(kms) >= 2:
            print(f"Telemetry reports during capture: {kms[-1] - kms[0]}")
        print(f"Raw capture saved to {logfile} — attach it to a GitHub issue if needed.")
    finally:
        try:
            if not args.keep_debug:
                send(ser, "km.debug(0)")
            send(ser, "km.telem(0)")
            time.sleep(0.2)
        except Exception:
            pass
        ser.close()


if __name__ == "__main__":
    main()
