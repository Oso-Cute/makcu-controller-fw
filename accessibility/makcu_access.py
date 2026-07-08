"""
makcu_access.py — accessibility control layer for the MAKCM passthrough firmware.

The firmware exposes a kmbox-style ASCII command channel over the Left MCU's
UART0 -> CH343 -> a COM port on this PC (4 Mbaud, 8N1). This module wraps that
channel for ACCESSIBILITY use: turning momentary taps into sustained holds
(latch/toggle), remapping, and simple macros — for players who can't physically
hold a button down.

It does NOT read the real controller; your real controller drives the game
normally through the passthrough. This layer only ADDS button holds on top,
triggered by whatever input source you choose (keyboard key, USB foot pedal,
accessibility switch, etc).

Requires: pip install pyserial
"""

import serial   # pyserial


# Every hold/release command the firmware actually implements, confirmed from
# km_inject.c. name -> (command_on, command_off)
BUTTONS = {
    "A":      ("km.btnA(1)",   "km.btnA(0)"),
    "B":      ("km.btnB(1)",   "km.btnB(0)"),
    "X":      ("km.btnX(1)",   "km.btnX(0)"),
    "Y":      ("km.btnY(1)",   "km.btnY(0)"),
    "LB":     ("km.lb(1)",     "km.lb(0)"),
    "RB":     ("km.rb(1)",     "km.rb(0)"),
    "RT":     ("km.left(1)",   "km.left(0)"),    # right trigger / "fire"
    "LT":     ("km.right(1)",  "km.right(0)"),   # left trigger / "ADS"
    "Xbtn":   ("km.middle(1)", "km.middle(0)"),  # alias path for X
}


class Makcu:
    def __init__(self, port, baud=4_000_000):
        # 8N1 is the firmware default; short timeout so reads never block long.
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self._held = set()   # names currently latched on

    def _send(self, line):
        self.ser.write((line + "\n").encode("ascii"))

    def version(self):
        """Handshake — proves the link works. Returns the kmbox id line."""
        self._send("km.version()")
        return self.ser.read(128).decode("ascii", "replace")

    # --- raw hold / release -------------------------------------------------
    def hold(self, name):
        on, _ = BUTTONS[name]
        self._send(on)
        self._held.add(name)

    def release(self, name):
        _, off = BUTTONS[name]
        self._send(off)
        self._held.discard(name)

    # --- the accessibility primitive: tap once = stays held -----------------
    def toggle(self, name):
        """Momentary tap flips a sustained hold on/off. This is the whole
        point: you never physically hold the button."""
        if name in self._held:
            self.release(name)
        else:
            self.hold(name)
        return name in self._held

    def release_all(self):
        for name in list(self._held):
            self.release(name)

    # --- right-stick nudge (delta only; no absolute aim in this firmware) ----
    def move(self, dx, dy):
        self._send(f"km.move({int(dx)},{int(dy)})")

    # --- accessibility: tremor-damp "steady" filter on the aim stick --------
    def steady(self, on):
        """Enable/disable the right-stick low-pass + deadzone."""
        self._send(f"km.steady({1 if on else 0})")

    def steady_smoothing(self, n):
        """0..99. Higher = smoother but more lag. ~70 start."""
        self._send(f"km.steady_a({max(0, min(99, int(n)))})")

    def steady_deadzone(self, n):
        """0..32000. Shake below this reads as center."""
        self._send(f"km.steady_d({max(0, min(32000, int(n)))})")

    def trim(self, x, y):
        """Constant right-stick offset (drift cancel / gentle pull). To cancel
        a resting drift, pass the NEGATIVE of the monitor's resting mean.
        Range +-32767. trim(0,0) clears it."""
        cl = lambda v: max(-32767, min(32767, int(v)))
        self._send(f"km.trim({cl(x)},{cl(y)})")

    # --- telemetry (physical sticks + buttons streamed back) ----------------
    def telem(self, on):
        self._send(f"km.telem({1 if on else 0})")

    def read_telem(self):
        """Yield parsed telemetry dicts from the 'KMS ...' lines the firmware
        streams when telem is on. Blocks on the serial read timeout."""
        buf = b""
        while True:
            buf += self.ser.read(256)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                s = line.decode("ascii", "replace").strip()
                if not s.startswith("KMS "):
                    continue
                d = {}
                for tok in s[4:].split():
                    k, _, v = tok.partition("=")
                    d[k] = int(v, 16) if k == "b" else int(v)
                if {"lx", "ly", "rx", "ry"} <= d.keys():
                    yield d


# ---------------------------------------------------------------------------
# Config is loaded from config.json (next to this file) so you never have to
# edit code to remap. Keys are NOT hard-coded — change them in config.json.
# If config.json is missing, these defaults are used and written out for you.
# ---------------------------------------------------------------------------
import json
import os

DEFAULT_CONFIG = {
    "port": "COM3",              # your CH343 COM port
    "pedal_map": {               # pedal key -> controller button to latch-toggle
        "f13": "RT",             # left pedal  -> right trigger (fire)
        "f14": "A",              # middle pedal-> A
        "f15": "LT",             # right pedal -> left trigger (ADS)
    },
}


def load_config():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.json")
    if not os.path.exists(path):
        with open(path, "w") as f:
            json.dump(DEFAULT_CONFIG, f, indent=2)
        print(f"wrote default config -> {path}  (edit it, no code needed)")
        return DEFAULT_CONFIG
    with open(path) as f:
        return json.load(f)


if __name__ == "__main__":
    # pip install pynput
    # A pedal that emits a keystroke is caught here exactly like a keyboard.
    from pynput import keyboard

    cfg = load_config()
    PORT = cfg["port"]
    PEDAL_MAP = {k.lower(): v for k, v in cfg["pedal_map"].items()}

    mk = Makcu(PORT)
    print("link:", mk.version().strip())
    print("pedals ->", PEDAL_MAP, " | esc = release all + quit")

    def _name(key):
        # normalize both printable keys ('a') and named keys (F13, esc)
        if hasattr(key, "char") and key.char is not None:
            return key.char.lower()
        return str(key).replace("Key.", "").lower()

    def on_press(key):
        name = _name(key)
        if name == "esc":
            mk.release_all()
            return False
        btn = PEDAL_MAP.get(name)
        if btn:
            state = mk.toggle(btn)
            print(f"{btn} {'HELD' if state else 'released'}  (pedal {name})")

    with keyboard.Listener(on_press=on_press) as l:
        l.join()
