"""
flash_tool.py — guided MAKCM flasher.

Walks the exact sequence:
  1. Connect the MIDDLE cable (USB2) -> confirm communication (km.version).
  2. Hold the LEFT (USB1) button + plug in -> Flash Left.
  3. Disconnect Left, plug in the RIGHT (USB3) -> Flash Right.
  4. Disconnect, then reconnect: PC->MIDDLE, Xbox->LEFT, controller->RIGHT.

Flashes the merged images built for the last working firmware set.

Requires: pip install pyserial   (tkinter + esptool come with the PlatformIO
Python / a normal Python + esptool install).
Run:      python flash_tool.py
"""

import os
import sys
import glob
import time
import threading
import subprocess

import tkinter as tk
from tkinter import ttk, filedialog, messagebox

import serial
import serial.tools.list_ports


# ---- where the merged flash images live --------------------------------------
# Default: firmware/bin/ in this repo. If a file is missing, the tool
# opens a file dialog so you can point it at your own build.
_HERE = os.path.dirname(os.path.abspath(__file__))
_DEFAULT_IMG_DIR = os.path.join(_HERE, "bin")
LEFT_BIN = os.path.join(_DEFAULT_IMG_DIR, "MERGED_left.bin")
RIGHT_BIN = os.path.join(_DEFAULT_IMG_DIR, "MERGED_right.bin")

# USB identities
CH343_VID = 0x1A86            # middle command port (USB2)
ESP_VID = 0x303A
DL_PID = 0x0009               # ROM download mode (BOOT held) — either MCU
RIGHT_PID = 0x1001            # Right MCU USB-Serial/JTAG (flashable directly)

KM_BAUD = 4_000_000
FLASH_BAUD = "921600"


def find_esptool():
    """Return an argv prefix that runs esptool, or None."""
    # 1) esptool as a module in the current interpreter (verify it actually runs)
    try:
        r = subprocess.run([sys.executable, "-m", "esptool", "version"],
                           capture_output=True, timeout=15)
        if r.returncode == 0:
            return [sys.executable, "-m", "esptool"]
    except Exception:
        pass
    # 2) the PlatformIO-bundled esptool.py
    home = os.path.expanduser("~")
    for p in glob.glob(os.path.join(home, ".platformio", "packages",
                                    "tool-esptoolpy*", "esptool.py")):
        return [sys.executable, p]
    return None


def find_port(vid, pid=None):
    for p in serial.tools.list_ports.comports():
        if p.vid == vid and (pid is None or p.pid == pid):
            return p.device
    return None


class FlashWizard:
    STEPS = [
        "1. Confirm communication",
        "2. Flash LEFT (USB1)",
        "3. Flash RIGHT (USB3)",
        "4. Reconnect for use",
    ]

    def __init__(self, root):
        self.root = root
        root.title("MAKCM Flash Tool")
        root.geometry("680x560")
        self.step = 0
        self.busy = False
        self.esptool = find_esptool()
        self.left_bin = LEFT_BIN
        self.right_bin = RIGHT_BIN

        top = ttk.Frame(root, padding=10)
        top.pack(fill="x")
        self.step_lbl = ttk.Label(top, text="", font=("Segoe UI", 13, "bold"))
        self.step_lbl.pack(anchor="w")
        self.instr = ttk.Label(top, text="", wraplength=640, justify="left")
        self.instr.pack(anchor="w", pady=(6, 0))

        mid = ttk.Frame(root, padding=(10, 0))
        mid.pack(fill="x")
        self.detect = ttk.Label(mid, text="", foreground="#2a6")
        self.detect.pack(anchor="w", pady=4)

        self.log = tk.Text(root, height=16, wrap="word", font=("Consolas", 9))
        self.log.pack(fill="both", expand=True, padx=10, pady=6)
        self.log.configure(state="disabled")

        btns = ttk.Frame(root, padding=10)
        btns.pack(fill="x", side="bottom")
        self.action_btn = ttk.Button(btns, text="", command=self.on_action)
        self.action_btn.pack(side="left")
        self.skip_btn = ttk.Button(btns, text="Skip ▶", command=self.on_skip,
                                   state="disabled")
        self.skip_btn.pack(side="left", padx=6)
        self.next_btn = ttk.Button(btns, text="Next ▶", command=self.on_next,
                                   state="disabled")
        self.next_btn.pack(side="right")
        self.back_btn = ttk.Button(btns, text="◀ Back", command=self.on_back)
        self.back_btn.pack(side="right", padx=6)

        if not self.esptool:
            self._log("WARNING: esptool not found. Install with: pip install esptool")
        for label, path in (("Left", self.left_bin), ("Right", self.right_bin)):
            if not os.path.exists(path):
                self._log(f"WARNING: {label} image missing: {path}")

        self.render()
        self.root.after(1000, self._poll)

    # ---------------------------------------------------------------- UI helpers
    def _log(self, msg):
        self.log.configure(state="normal")
        self.log.insert("end", msg + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def render(self):
        self.step_lbl.config(text=self.STEPS[self.step])
        self.next_btn.config(state="disabled")
        self.back_btn.config(state="normal" if self.step > 0 and not self.busy
                             else "disabled")
        self.skip_btn.config(state="normal" if self.step in (1, 2) and not self.busy
                             else "disabled")
        self.detect.config(text="")
        if self.step == 0:
            self.instr.config(text=(
                "Plug ONLY the MIDDLE cable (USB2) into this PC.\n"
                "This just confirms the MAKCU is detected (its CH343 command "
                "port shows up). One cable — nothing else needed yet.\n\n"
                "Click ‘Detect MAKCU’."))
            self.action_btn.config(text="Detect MAKCU", state="normal")
        elif self.step == 1:
            self.instr.config(text=(
                "Now flash the LEFT MCU.\n"
                "1) HOLD the button next to USB1 (left).\n"
                "2) While holding, plug USB1 into this PC.\n"
                "3) Release. Wait for it to be detected below, then click "
                "‘Flash Left’.\n\n"
                "Already up to date on this side? Click ‘Skip ▶’."))
            self.action_btn.config(text="Flash Left", state="disabled")
        elif self.step == 2:
            self.instr.config(text=(
                "Left done. Now the RIGHT MCU.\n"
                "1) Unplug USB1.\n"
                "2) Plug USB3 (right) into this PC.\n"
                "3) If it is NOT detected within a few seconds: unplug USB3, "
                "HOLD the BOOT button next to USB3, plug it back in, release. "
                "(Needed when the Right MCU is already running USB-host "
                "firmware — the port stays silent otherwise.)\n"
                "4) Wait for detection below, then click ‘Flash Right’.\n\n"
                "Already up to date on this side? Click ‘Skip ▶’."))
            self.action_btn.config(text="Flash Right", state="disabled")
        elif self.step == 3:
            self.instr.config(text=(
                "All flashed! Reconnect for normal use:\n"
                "• PC  → MIDDLE (USB2)\n"
                "• Xbox → LEFT (USB1)\n"
                "• Controller → RIGHT (USB3)\n\n"
                "Then power-cycle if needed. Click ‘Verify’ to confirm the "
                "command port answers, or ‘Finish’."))
            self.action_btn.config(text="Verify", state="normal")
            self.next_btn.config(text="Finish", state="normal")

    def _poll(self):
        """Auto-detect the relevant port for flashing steps."""
        if not self.busy:
            if self.step == 1:
                port = find_port(ESP_VID, DL_PID)
                if port:
                    self.detect.config(text=f"Left detected in download mode on {port}")
                    self.action_btn.config(state="normal")
                    self._left_port = port
                else:
                    self.detect.config(text="Waiting for Left (hold USB1 button + plug in)…")
                    self.action_btn.config(state="disabled")
            elif self.step == 2:
                port = find_port(ESP_VID, RIGHT_PID)
                dl_port = None if port else find_port(ESP_VID, DL_PID)
                if port:
                    self.detect.config(text=f"Right detected on {port}")
                    self.action_btn.config(state="normal")
                    self._right_port = port
                elif dl_port:
                    self.detect.config(
                        text=f"ESP32 in download mode on {dl_port} — if USB1 is "
                             "unplugged this is the Right MCU; click Flash Right")
                    self.action_btn.config(state="normal")
                    self._right_port = dl_port
                else:
                    self.detect.config(
                        text="Waiting for Right (plug USB3 into PC; if nothing "
                             "appears, hold the BOOT button next to USB3 while "
                             "plugging in)…")
                    self.action_btn.config(state="disabled")
        self.root.after(1000, self._poll)

    # ------------------------------------------------------------------ actions
    def on_action(self):
        if self.busy:
            return
        if self.step == 0:
            self.detect_makcu()
        elif self.step == 1:
            self.flash(self.left_bin, getattr(self, "_left_port", None), "Left")
        elif self.step == 2:
            self.flash(self.right_bin, getattr(self, "_right_port", None), "Right")
        elif self.step == 3:
            self.check_comm(final=True)

    def detect_makcu(self):
        """Step 1: just confirm the MAKCU's CH343 command port is present.
        No handshake, no power needed — one cable (the middle) only."""
        port = find_port(CH343_VID)
        if port:
            self.detect.config(text=f"MAKCU detected — CH343 command port on {port}.")
            self._log(f"MAKCU detected on {port}. Ready to flash.")
            self.next_btn.config(state="normal")
        else:
            self.detect.config(text="No MAKCU found. Plug the MIDDLE cable (USB2) "
                               "into this PC and click Detect again.")
            self._log("No CH343 command port detected.")

    def check_comm(self, final=False):
        port = find_port(CH343_VID)
        if not port:
            self.detect.config(text="No CH343 (middle port) found — plug USB2 into PC.")
            self._log("Communication check: CH343 command port not found.")
            return
        try:
            s = serial.Serial(port, KM_BAUD, timeout=1.0)
            time.sleep(0.3)
            s.reset_input_buffer()
            s.write(b"km.version()\n")
            time.sleep(0.4)
            reply = s.read(80).decode("ascii", "replace").strip()
            s.close()
        except Exception as e:
            self._log(f"Comm check error on {port}: {e}")
            return
        if reply.startswith("kmbox"):
            self.detect.config(text=f"Communication OK on {port}: {reply.splitlines()[0]}")
            self._log(f"Comm OK ({port}): {reply}")
            if not final:
                self.next_btn.config(state="normal")
        else:
            self.detect.config(text=f"Port {port} open but no kmbox reply "
                               f"(is USB1 powering the Left MCU?)")
            self._log(f"Comm check: unexpected reply {reply!r}")

    def flash(self, binfile, port, name):
        if not self.esptool:
            messagebox.showerror("esptool missing", "Install esptool: pip install esptool")
            return
        if not port:
            messagebox.showwarning("No port", f"{name} not detected yet.")
            return
        if not os.path.exists(binfile):
            path = filedialog.askopenfilename(
                title=f"Locate MERGED_{name.lower()}.bin",
                filetypes=[("bin", "*.bin")])
            if not path:
                return
            binfile = path
        self.busy = True
        self.action_btn.config(state="disabled")
        self.back_btn.config(state="disabled")
        self.skip_btn.config(state="disabled")
        self._log(f"\n=== Flashing {name} on {port} ===")
        cmd = self.esptool + ["--chip", "esp32s3", "--port", port,
                              "--baud", FLASH_BAUD, "write_flash", "0x0", binfile]
        threading.Thread(target=self._flash_worker, args=(cmd, name), daemon=True).start()

    def _flash_worker(self, cmd, name):
        ok = False
        try:
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True,
                                    bufsize=1)
            for line in proc.stdout:
                line = line.rstrip()
                if line:
                    self.root.after(0, self._log, line)
                if "Hash of data verified" in line:
                    ok = True
            proc.wait()
        except Exception as e:
            self.root.after(0, self._log, f"esptool error: {e}")
        self.root.after(0, self._flash_done, name, ok)

    def _flash_done(self, name, ok):
        self.busy = False
        self.back_btn.config(state="normal")
        self.skip_btn.config(state="normal" if self.step in (1, 2) else "disabled")
        if ok:
            self._log(f"=== {name} flashed OK. "
                      f"(‘can not exit download mode’ / Error 1 is normal.) ===")
            self.detect.config(text=f"{name} flashed successfully — click Next.")
            self.next_btn.config(state="normal")
        else:
            self._log(f"=== {name} flash did NOT verify. Check the cable/port and retry. ===")
            self.action_btn.config(state="normal")

    def on_skip(self):
        """Skip a flash step (e.g. that MCU already runs the current image)."""
        if self.busy or self.step not in (1, 2):
            return
        name = "Left" if self.step == 1 else "Right"
        self._log(f"{name} flash SKIPPED — that MCU keeps whatever it is running.")
        self.step += 1
        self.render()

    def on_next(self):
        if self.step < len(self.STEPS) - 1:
            self.step += 1
            self.render()
        else:
            self.root.destroy()

    def on_back(self):
        if self.step > 0 and not self.busy:
            self.step -= 1
            self.render()


def main():
    root = tk.Tk()
    try:
        ttk.Style().theme_use("clam")
    except Exception:
        pass
    FlashWizard(root)
    root.mainloop()


if __name__ == "__main__":
    main()
