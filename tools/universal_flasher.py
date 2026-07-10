"""
universal_flasher.py — flash any .bin to an ESP32-S3 MCU (Left or Right).

Small GUI: pick (or drag-drop) a .bin, pick the COM port, click Flash.
Wraps esptool exactly like flash_tool.py does:

    esptool --chip esp32s3 --port <COM> --baud <baud> write_flash <offset> <bin>

Safety rails:
  - refuses to flash the CH343 command port (middle USB — no auto-reset),
  - checks the image header: merged/bootloader images start with 0xE9 and
    must be DIO (byte 2 == 0x02) — a QIO image will not boot on this board,
  - offset defaults to 0x0 (right for the MERGED_*.bin images; use 0x10000
    for an app-only firmware.bin).

Requires: pip install pyserial esptool
Optional: pip install tkinterdnd2   (enables drag & drop onto the window)
Run:      python tools/universal_flasher.py
"""

import os
import sys
import glob
import threading
import subprocess

import tkinter as tk
from tkinter import ttk, filedialog, messagebox

import serial.tools.list_ports

CH343_VID = 0x1A86
ESP_VID = 0x303A

# Optional drag & drop support.
try:
    from tkinterdnd2 import TkinterDnD, DND_FILES
    _HAVE_DND = True
except ImportError:
    _HAVE_DND = False


def find_esptool():
    """Return an argv prefix that runs esptool, or None."""
    try:
        r = subprocess.run([sys.executable, "-m", "esptool", "version"],
                           capture_output=True, timeout=15)
        if r.returncode == 0:
            return [sys.executable, "-m", "esptool"]
    except Exception:
        pass
    home = os.path.expanduser("~")
    for p in glob.glob(os.path.join(home, ".platformio", "packages",
                                    "tool-esptoolpy*", "esptool.py")):
        return [sys.executable, p]
    return None


def describe_port(p):
    """Human-readable one-liner for a comport entry."""
    vid = f"{p.vid:04X}" if p.vid else "----"
    pid = f"{p.pid:04X}" if p.pid else "----"
    tag = ""
    if p.vid == CH343_VID:
        tag = "  [CH343 command port — DO NOT FLASH]"
    elif p.vid == ESP_VID and p.pid == 0x0009:
        tag = "  [ESP32 download mode — ready to flash]"
    elif p.vid == ESP_VID and p.pid == 0x1001:
        tag = "  [ESP32 USB-Serial/JTAG — ready to flash]"
    return f"{p.device}  VID:PID={vid}:{pid}{tag}"


class UniversalFlasher:
    def __init__(self, root):
        self.root = root
        root.title("MAKCM Universal Flasher")
        root.geometry("720x560")
        self.esptool = find_esptool()
        self.busy = False
        self.ports = []

        frm = ttk.Frame(root, padding=10)
        frm.pack(fill="x")

        # --- bin picker -------------------------------------------------
        ttk.Label(frm, text="Firmware image (.bin):",
                  font=("Segoe UI", 10, "bold")).grid(row=0, column=0, sticky="w")
        self.bin_var = tk.StringVar()
        self.bin_entry = ttk.Entry(frm, textvariable=self.bin_var, width=70)
        self.bin_entry.grid(row=1, column=0, columnspan=2, sticky="we", pady=2)
        ttk.Button(frm, text="Browse…", command=self.browse).grid(
            row=1, column=2, padx=(6, 0))
        drop_hint = "…or drag & drop a .bin anywhere on this window" if _HAVE_DND \
            else "(drag & drop available with: pip install tkinterdnd2)"
        ttk.Label(frm, text=drop_hint, foreground="#888").grid(
            row=2, column=0, columnspan=3, sticky="w")
        self.bin_info = ttk.Label(frm, text="", foreground="#26a")
        self.bin_info.grid(row=3, column=0, columnspan=3, sticky="w", pady=(0, 6))

        # --- port picker ------------------------------------------------
        ttk.Label(frm, text="Port:", font=("Segoe UI", 10, "bold")).grid(
            row=4, column=0, sticky="w")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(frm, textvariable=self.port_var,
                                       state="readonly", width=68)
        self.port_combo.grid(row=5, column=0, columnspan=2, sticky="we", pady=2)
        ttk.Button(frm, text="Refresh", command=self.refresh_ports).grid(
            row=5, column=2, padx=(6, 0))
        ttk.Label(frm, foreground="#888", text=(
            "Left MCU: hold BOOT next to USB1 while plugging into the PC.  "
            "Right MCU: plug USB3\n(hold its BOOT button if nothing shows up). "
            "One MCU cable at a time.")).grid(
            row=6, column=0, columnspan=3, sticky="w")

        # --- options ----------------------------------------------------
        opts = ttk.Frame(frm)
        opts.grid(row=7, column=0, columnspan=3, sticky="w", pady=(8, 0))
        ttk.Label(opts, text="Offset:").pack(side="left")
        self.offset_var = tk.StringVar(value="0x0")
        ttk.Combobox(opts, textvariable=self.offset_var, width=10,
                     values=["0x0", "0x10000"]).pack(side="left", padx=(4, 16))
        ttk.Label(opts, text="Baud:").pack(side="left")
        self.baud_var = tk.StringVar(value="921600")
        ttk.Combobox(opts, textvariable=self.baud_var, width=10, state="readonly",
                     values=["921600", "460800", "115200"]).pack(side="left", padx=4)
        ttk.Label(opts, text="(0x0 = MERGED images; 0x10000 = app-only firmware.bin)",
                  foreground="#888").pack(side="left", padx=8)

        self.flash_btn = ttk.Button(frm, text="Flash", command=self.on_flash)
        self.flash_btn.grid(row=8, column=0, sticky="w", pady=8)
        self.status = ttk.Label(frm, text="", foreground="#2a6")
        self.status.grid(row=8, column=1, columnspan=2, sticky="w")

        # --- log --------------------------------------------------------
        self.log = tk.Text(root, height=18, wrap="word", font=("Consolas", 9))
        self.log.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        self.log.configure(state="disabled")

        if not self.esptool:
            self._log("WARNING: esptool not found. Install with: pip install esptool")
        if _HAVE_DND:
            root.drop_target_register(DND_FILES)
            root.dnd_bind("<<Drop>>", self.on_drop)

        self.refresh_ports()
        self.root.after(2000, self._auto_refresh)

    # ------------------------------------------------------------------ UI
    def _log(self, msg):
        self.log.configure(state="normal")
        self.log.insert("end", msg + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def browse(self):
        path = filedialog.askopenfilename(
            title="Select firmware .bin",
            filetypes=[("Firmware Binary", "*.bin"), ("All Files", "*.*")])
        if path:
            self.set_bin(path)

    def on_drop(self, event):
        # tkinterdnd2 wraps paths containing spaces in braces
        path = event.data.strip()
        if path.startswith("{") and path.endswith("}"):
            path = path[1:-1]
        if path.lower().endswith(".bin"):
            self.set_bin(path)
        else:
            self._log(f"Ignored drop (not a .bin): {path}")

    def set_bin(self, path):
        self.bin_var.set(path)
        try:
            with open(path, "rb") as f:
                hdr = f.read(4)
            size = os.path.getsize(path)
        except OSError as e:
            self.bin_info.config(text=f"Cannot read file: {e}", foreground="#c33")
            return
        if len(hdr) == 4 and hdr[0] == 0xE9:
            mode = {0: "QIO", 1: "QOUT", 2: "DIO", 3: "DOUT"}.get(hdr[2], "?")
            color = "#26a" if hdr[2] == 2 else "#c33"
            warn = "" if hdr[2] == 2 else "  — WARNING: not DIO, this board won't boot it!"
            self.bin_info.config(
                text=f"ESP image header OK ({mode}, {size:,} bytes){warn}",
                foreground=color)
            if self.offset_var.get() != "0x0":
                self._log("NOTE: image starts with an ESP header — merged/bootloader "
                          "images belong at offset 0x0.")
        else:
            self.bin_info.config(
                text=f"No ESP image header at byte 0 ({size:,} bytes) — app-only "
                     "image? Use offset 0x10000.", foreground="#c60")

    def refresh_ports(self):
        self.ports = list(serial.tools.list_ports.comports())
        labels = [describe_port(p) for p in self.ports]
        self.port_combo["values"] = labels
        # Auto-select the first flashable ESP port if nothing is chosen.
        if not self.port_var.get():
            for i, p in enumerate(self.ports):
                if p.vid == ESP_VID:
                    self.port_combo.current(i)
                    break

    def _auto_refresh(self):
        if not self.busy:
            sel = self.port_var.get()
            self.refresh_ports()
            if sel in self.port_combo["values"]:
                self.port_var.set(sel)
        self.root.after(2000, self._auto_refresh)

    # --------------------------------------------------------------- flash
    def on_flash(self):
        if self.busy:
            return
        if not self.esptool:
            messagebox.showerror("esptool missing",
                                 "Install esptool: pip install esptool")
            return
        binfile = self.bin_var.get().strip()
        if not binfile or not os.path.isfile(binfile):
            messagebox.showwarning("No file", "Select a .bin file first.")
            return
        idx = self.port_combo.current()
        if idx < 0 or idx >= len(self.ports):
            messagebox.showwarning("No port", "Select a COM port.")
            return
        port = self.ports[idx]
        if port.vid == CH343_VID:
            messagebox.showerror(
                "Wrong port",
                "That is the CH343 command port (middle USB). It has no "
                "auto-reset wiring — flashing over it does not work.\n\n"
                "Use USB1 (hold BOOT while plugging in) or USB3.")
            return
        offset = self.offset_var.get().strip() or "0x0"
        try:
            int(offset, 16)
        except ValueError:
            messagebox.showwarning("Bad offset", f"Not a hex offset: {offset}")
            return

        self.busy = True
        self.flash_btn.config(state="disabled")
        self.status.config(text=f"Flashing {os.path.basename(binfile)} → "
                                f"{port.device} @ {offset}…")
        self._log(f"\n=== esptool write_flash {offset} {binfile} "
                  f"(port {port.device}, baud {self.baud_var.get()}) ===")
        cmd = self.esptool + ["--chip", "esp32s3", "--port", port.device,
                              "--baud", self.baud_var.get(),
                              "write_flash", offset, binfile]
        threading.Thread(target=self._worker, args=(cmd,), daemon=True).start()

    def _worker(self, cmd):
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
        self.root.after(0, self._done, ok)

    def _done(self, ok):
        self.busy = False
        self.flash_btn.config(state="normal")
        if ok:
            self.status.config(text="Flash SUCCEEDED — power-cycle the board.",
                               foreground="#2a6")
            self._log("=== SUCCESS. ('can not exit download mode' / Error 1 after "
                      "this is normal.) Unplug everything, wait 5 s, reconnect. ===")
        else:
            self.status.config(text="Flash FAILED — see log.", foreground="#c33")
            self._log("=== FAILED — no 'Hash of data verified'. Check port/cable, "
                      "redo the BOOT-button sequence, or drop baud to 115200. ===")


def main():
    root = TkinterDnD.Tk() if _HAVE_DND else tk.Tk()
    try:
        ttk.Style().theme_use("clam")
    except Exception:
        pass
    UniversalFlasher(root)
    root.mainloop()


if __name__ == "__main__":
    main()
