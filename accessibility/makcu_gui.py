"""
makcu_gui.py — accessibility control panel for the MAKCM controller passthrough.

A tabbed GUI over the same km.* command channel the CLI uses:
  - Device : detect serial ports, flag the CH343 command port, connect + handshake
  - Steady : tremor-damp filter (low-pass + deadzone) on the aim stick
  - Trim   : constant stick offset to cancel drift
  - Monitor: live stick/button readout, measure tremor, apply recommendations
  - Buttons: latch/toggle holds (tap once = held) for one-handed / no-long-hold use
  - Test   : manual hardware validation — LB/RB spam test (never runs on its own)

Requires: pip install pyserial   (tkinter ships with Python on Windows)
Run:      python makcu_gui.py
"""

import threading
import queue
import time
import re
import statistics

import tkinter as tk
from tkinter import ttk, messagebox

import serial
import serial.tools.list_ports

from makcu_access import Makcu, BUTTONS


KM_BAUD = 4_000_000
CH343_VID = 0x1A86          # WCH CH343 — the MAKCM command (USB2) bridge
ESP_VID = 0x303A            # Espressif native USB (a MAKCM MCU in some mode)


def classify_port(p):
    """Return a human hint for what a serial port probably is."""
    vid = p.vid or 0
    if vid == CH343_VID:
        return "CH343 — MAKCM command port (use this)"
    if vid == ESP_VID:
        return "ESP32-S3 native USB (MCU, not the command port)"
    return "other"


class MakcuGUI:
    def __init__(self, root):
        self.root = root
        root.title("MAKCM Accessibility Panel")
        root.geometry("640x560")

        self.mk = None                 # Makcu instance when connected
        self.reader = None             # background telemetry reader thread
        self.reader_stop = threading.Event()
        self.tel_q = queue.Queue()     # parsed telemetry dicts -> UI
        self.io_lock = threading.Lock()
        self.latched = {}              # button name -> bool
        self._measuring = False
        self._measure_samples = []

        self.nb = ttk.Notebook(root)
        self.nb.pack(fill="both", expand=True, padx=8, pady=8)
        self._build_device_tab()
        self._build_steady_tab()
        self._build_trim_tab()
        self._build_monitor_tab()
        self._build_buttons_tab()
        self._build_test_tab()

        self.status = tk.StringVar(value="Not connected.")
        ttk.Label(root, textvariable=self.status, relief="sunken",
                  anchor="w").pack(fill="x", side="bottom")

        self.refresh_ports()
        self.root.after(60, self._drain_telemetry)

    # ------------------------------------------------------------------ Device
    def _build_device_tab(self):
        f = ttk.Frame(self.nb)
        self.nb.add(f, text="Device")

        ttk.Label(f, text="Detected serial ports:").pack(anchor="w", pady=(6, 2))
        cols = ("port", "desc", "vidpid", "hint")
        self.tree = ttk.Treeview(f, columns=cols, show="headings", height=7)
        for c, w in zip(cols, (70, 190, 90, 250)):
            self.tree.heading(c, text=c.upper())
            self.tree.column(c, width=w, anchor="w")
        self.tree.pack(fill="x", pady=4)

        row = ttk.Frame(f)
        row.pack(fill="x", pady=6)
        ttk.Button(row, text="Refresh", command=self.refresh_ports).pack(side="left")
        ttk.Button(row, text="Auto-detect CH343", command=self.autodetect).pack(side="left", padx=6)

        row2 = ttk.Frame(f)
        row2.pack(fill="x", pady=6)
        ttk.Label(row2, text="Port:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_entry = ttk.Entry(row2, textvariable=self.port_var, width=14)
        self.port_entry.pack(side="left", padx=6)
        self.connect_btn = ttk.Button(row2, text="Connect", command=self.toggle_connect)
        self.connect_btn.pack(side="left")
        ttk.Button(row2, text="Handshake test", command=self.handshake).pack(side="left", padx=6)

        self.dev_info = tk.StringVar(value="Select a port, then Connect.")
        ttk.Label(f, textvariable=self.dev_info, foreground="#2a6",
                  wraplength=580, justify="left").pack(anchor="w", pady=8)

        self.tree.bind("<<TreeviewSelect>>", self._on_port_select)

    def refresh_ports(self):
        self.tree.delete(*self.tree.get_children())
        best = None
        for p in serial.tools.list_ports.comports():
            hint = classify_port(p)
            vidpid = f"{p.vid:04X}:{p.pid:04X}" if p.vid else "-"
            self.tree.insert("", "end", values=(p.device, p.description, vidpid, hint))
            if p.vid == CH343_VID and best is None:
                best = p.device
        if best and not self.port_var.get():
            self.port_var.set(best)

    def autodetect(self):
        for p in serial.tools.list_ports.comports():
            if p.vid == CH343_VID:
                self.port_var.set(p.device)
                self.dev_info.set(f"Found CH343 command port at {p.device}. Click Connect.")
                return
        self.dev_info.set("No CH343 (VID 1A86) found. Is USB2 plugged into this PC, "
                          "and USB1 powering the Left MCU?")

    def _on_port_select(self, _evt):
        sel = self.tree.selection()
        if sel:
            self.port_var.set(self.tree.item(sel[0], "values")[0])

    def toggle_connect(self):
        if self.mk:
            self.disconnect()
        else:
            self.connect()

    def connect(self):
        port = self.port_var.get().strip()
        if not port:
            messagebox.showwarning("No port", "Pick a port first (or Auto-detect).")
            return
        try:
            self.mk = Makcu(port)
        except Exception as e:
            messagebox.showerror("Connect failed", str(e))
            self.mk = None
            return
        self.connect_btn.config(text="Disconnect")
        self.status.set(f"Connected on {port}.")
        self.handshake()
        self._start_reader()

    def disconnect(self):
        self._stop_reader()
        if self.mk:
            try:
                self.mk.telem(False)
                self.mk.release_all()
                self.mk.ser.close()
            except Exception:
                pass
        self.mk = None
        self.connect_btn.config(text="Connect")
        self.status.set("Disconnected.")

    def handshake(self):
        if not self.mk:
            self.dev_info.set("Not connected.")
            return
        try:
            # Pause the telemetry reader — it reads the same port and would
            # otherwise swallow the version reply (the button then shows junk).
            reader_was_running = self.reader is not None
            if reader_was_running:
                self._stop_reader()
                time.sleep(0.15)
            with self.io_lock:
                v = self.mk.version().strip()
            if reader_was_running:
                self._start_reader()
            if v.startswith("kmbox"):
                self.dev_info.set(f"OK — firmware responded:\n{v}")
                self.status.set("Handshake OK.")
            else:
                self.dev_info.set(f"Unexpected reply: {v!r}. Wrong port, or Left MCU "
                                  "not powered (USB1) / not the CH343 port.")
        except Exception as e:
            self.dev_info.set(f"Handshake error: {e}")

    def _slider(self, parent, label, var, lo, hi, cb):
        """A labeled horizontal slider that calls cb() on release + shows value."""
        row = ttk.Frame(parent)
        row.pack(fill="x", pady=4)
        ttk.Label(row, text=label, width=32, anchor="w").pack(side="left")
        val = ttk.Label(row, textvariable=var, width=7)
        val.pack(side="right")
        s = ttk.Scale(row, from_=lo, to=hi, orient="horizontal",
                      command=lambda v: var.set(int(float(v))))
        s.set(var.get())
        s.pack(side="left", fill="x", expand=True, padx=6)
        s.bind("<ButtonRelease-1>", lambda _e: cb())
        return s

    # ------------------------------------------------------------------ Steady
    def _build_steady_tab(self):
        f = ttk.Frame(self.nb)
        self.nb.add(f, text="Steady")
        ttk.Label(f, text="Tremor-damping filter on the aim (right) stick.",
                  wraplength=580).pack(anchor="w", pady=(8, 4))

        self.steady_on = tk.BooleanVar(value=False)
        ttk.Checkbutton(f, text="Enable steady filter", variable=self.steady_on,
                        command=self._apply_steady).pack(anchor="w", pady=4)

        self.smooth = tk.IntVar(value=70)
        self._slider(f, "Smoothing (0=off, 99=max lag)", self.smooth, 0, 99,
                     self._apply_steady)
        self.dead = tk.IntVar(value=6000)
        self._slider(f, "Deadzone (ignore shake below)", self.dead, 0, 20000,
                     self._apply_steady)

        ttk.Label(f, text="Tip: measure your shake in the Monitor tab first, then "
                  "let it apply a recommended starting point.",
                  wraplength=580, foreground="#666").pack(anchor="w", pady=10)

    def _apply_steady(self, *_):
        if not self.mk:
            return
        with self.io_lock:
            self.mk.steady_smoothing(self.smooth.get())
            self.mk.steady_deadzone(self.dead.get())
            self.mk.steady(self.steady_on.get())
        self.status.set(f"Steady {'ON' if self.steady_on.get() else 'off'} "
                        f"smooth={self.smooth.get()} dead={self.dead.get()}")

    # -------------------------------------------------------------------- Trim
    def _build_trim_tab(self):
        f = ttk.Frame(self.nb)
        self.nb.add(f, text="Trim")
        ttk.Label(f, text="Constant right-stick offset — cancel drift or add a gentle pull.",
                  wraplength=580).pack(anchor="w", pady=(8, 6))
        self.trim_x = tk.IntVar(value=0)
        self.trim_y = tk.IntVar(value=0)
        self._slider(f, "Trim X (left / right)", self.trim_x, -20000, 20000, self._apply_trim)
        self._slider(f, "Trim Y (up / down)", self.trim_y, -20000, 20000, self._apply_trim)
        row = ttk.Frame(f); row.pack(fill="x", pady=8)
        ttk.Button(row, text="Apply", command=self._apply_trim).pack(side="left")
        ttk.Button(row, text="Clear", command=self._clear_trim).pack(side="left", padx=6)

    def _apply_trim(self, *_):
        if not self.mk:
            return
        with self.io_lock:
            self.mk.trim(self.trim_x.get(), self.trim_y.get())
        self.status.set(f"Trim ({self.trim_x.get()},{self.trim_y.get()})")

    def _clear_trim(self):
        self.trim_x.set(0); self.trim_y.set(0)
        self._apply_trim()

    # ----------------------------------------------------------------- Monitor
    def _build_monitor_tab(self):
        f = ttk.Frame(self.nb)
        self.nb.add(f, text="Monitor")
        self.mon_live = tk.BooleanVar(value=False)
        ttk.Checkbutton(f, text="Live telemetry", variable=self.mon_live,
                        command=self._toggle_live).pack(anchor="w", pady=6)

        grid = ttk.Frame(f); grid.pack(anchor="w", pady=4)
        self.mon_vars = {}
        for i, k in enumerate(("lx", "ly", "rx", "ry", "b")):
            ttk.Label(grid, text=k.upper()+":", width=4).grid(row=i, column=0, sticky="e")
            v = tk.StringVar(value="—")
            self.mon_vars[k] = v
            ttk.Label(grid, textvariable=v, width=10, anchor="w",
                      font=("Consolas", 11)).grid(row=i, column=1, sticky="w")

        # Live button indicator grid — names come from tools/button_map.json.
        self.btn_map = self._load_button_map()
        self.mon_btn_labels = {}
        if self.btn_map:
            ttk.Label(f, text="Buttons (live):").pack(anchor="w", pady=(8, 2))
            bgrid = ttk.Frame(f)
            bgrid.pack(anchor="w")
            for i, name in enumerate(self.btn_map):
                lbl = tk.Label(bgrid, text=name, width=11, relief="ridge",
                               font=("Segoe UI", 9), bg="#e8e8e8", fg="#888")
                lbl.grid(row=i // 5, column=i % 5, padx=3, pady=3)
                self.mon_btn_labels[name] = lbl
        else:
            ttk.Label(f, text="(No button map — run tools/button_mapper.py to "
                      "name the b: bits.)", foreground="#666").pack(anchor="w")

        ttk.Button(f, text="Measure shake (5s) + recommend",
                   command=self.measure_shake).pack(anchor="w", pady=10)
        self.mon_result = tk.StringVar(value="")
        ttk.Label(f, textvariable=self.mon_result, wraplength=580,
                  justify="left", foreground="#2a6").pack(anchor="w")
        self._last_tel = {}

    def _load_button_map(self):
        """name -> bit mask, from tools/button_map.json if it exists."""
        import json, os
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "tools", "button_map.json")
        try:
            with open(path) as fh:
                return {k: int(v, 16) for k, v in json.load(fh).items()}
        except Exception:
            return {}

    def _toggle_live(self):
        if not self.mk:
            self.mon_live.set(False)
            return
        with self.io_lock:
            self.mk.telem(self.mon_live.get())

    def measure_shake(self):
        if not self.mk:
            messagebox.showwarning("Not connected", "Connect first.")
            return
        self.mon_result.set("Hold the stick as steady as you can… collecting 5s.")
        with self.io_lock:
            self.mk.telem(True)
        self._measure_samples = []
        self._measuring = True
        # drain loop collects into _measure_samples; stop + compute after 5s
        self.root.after(5000, self._finish_measure)

    def _finish_measure(self):
        self._measuring = False
        samples = list(self._measure_samples)
        if not self.mon_live.get():
            with self.io_lock:
                self.mk.telem(False)
        if not samples:
            self.mon_result.set("No telemetry — is the controller plugged in and streaming?")
            return
        def stat(axis):
            v = [s[axis] for s in samples]
            return max(v) - min(v), statistics.fmean(v)
        rxp, rxm = stat("rx"); ryp, rym = stat("ry")
        shake = max(rxp, ryp)
        dead = min(20000, int(shake * 0.6))
        alpha = 60 if shake < 4000 else 75 if shake < 12000 else 85
        tx, ty = round(rxm), round(rym)
        msg = (f"Shake peak-to-peak: X={rxp}  Y={ryp}\n"
               f"Applied → smoothing={alpha}, deadzone={dead}")
        self.smooth.set(alpha)
        self.dead.set(dead)
        if abs(tx) > 1500 or abs(ty) > 1500:
            msg += f"\nDrift detected ({tx:+d},{ty:+d}) → trim set to {-tx},{-ty}"
            self.trim_x.set(-tx)
            self.trim_y.set(-ty)
        self.mon_result.set(msg)

    # ----------------------------------------------------------------- Buttons
    def _build_buttons_tab(self):
        f = ttk.Frame(self.nb)
        self.nb.add(f, text="Buttons")
        ttk.Label(f, text="Latch toggles — tap once to HOLD the button, tap again to release. "
                  "(For one-handed / can't-hold-long use.)",
                  wraplength=580).pack(anchor="w", pady=(8, 8))
        grid = ttk.Frame(f); grid.pack(anchor="w")
        self.btn_state = {}
        names = list(BUTTONS.keys())
        for i, name in enumerate(names):
            var = tk.BooleanVar(value=False)
            self.btn_state[name] = var
            cb = ttk.Checkbutton(grid, text=name, variable=var,
                                 command=lambda n=name: self._toggle_btn(n))
            cb.grid(row=i // 3, column=i % 3, sticky="w", padx=10, pady=6)
        ttk.Button(f, text="Release all", command=self._release_all_btns).pack(anchor="w", pady=10)

    def _toggle_btn(self, name):
        if not self.mk:
            self.btn_state[name].set(False)
            return
        with self.io_lock:
            if self.btn_state[name].get():
                self.mk.hold(name)
            else:
                self.mk.release(name)
        self.status.set(f"{name} {'HELD' if self.btn_state[name].get() else 'released'}")

    def _release_all_btns(self):
        if self.mk:
            with self.io_lock:
                self.mk.release_all()
        for v in self.btn_state.values():
            v.set(False)

    # -------------------------------------------------------------------- Test
    def _build_test_tab(self):
        f = ttk.Frame(self.nb)
        self.nb.add(f, text="Test")
        ttk.Label(f, text="Manual hardware validation. Tests run ONLY when you "
                  "click a Start button.\n"
                  "Tip: GIP controllers only send USB reports on change — wiggle "
                  "a stick slightly during a test so injection has reports to "
                  "ride on.",
                  wraplength=580, justify="left").pack(anchor="w", pady=(8, 6))

        row = ttk.Frame(f)
        row.pack(fill="x", pady=4)
        ttk.Label(row, text="Duration (s):").pack(side="left")
        self.test_dur = tk.StringVar(value="4")
        ttk.Entry(row, textvariable=self.test_dur, width=6).pack(side="left", padx=6)
        self.test1_btn = ttk.Button(row, text="Test 1: LB/RB spam",
                                    command=lambda: self._start_test(1))
        self.test1_btn.pack(side="left", padx=6)
        self.test2_btn = ttk.Button(row, text="Test 2: aim-stick sweep",
                                    command=lambda: self._start_test(2))
        self.test2_btn.pack(side="left")
        self.test_stop_btn = ttk.Button(row, text="STOP", command=self._stop_test,
                                        state="disabled")
        self.test_stop_btn.pack(side="left", padx=6)

        ttk.Label(f, text="Test 1: presses+releases LB and RB (0.6 s held, "
                  "~1×/s — console-visible timing). "
                  "Test 2: slams the aim (right) stick full LEFT / full RIGHT "
                  "every 0.4 s via km.trim. (The firmware can only inject the "
                  "right stick — left-stick injection doesn't exist in "
                  "km_inject.c yet.)",
                  wraplength=580, justify="left",
                  foreground="#666").pack(anchor="w", pady=(0, 4))

        self.test_log = tk.Text(f, height=11, wrap="word", font=("Consolas", 9),
                                state="disabled")
        self.test_log.pack(fill="both", expand=True, pady=6)

        self.test_stop_evt = threading.Event()
        self._test_running = False

    def _tlog(self, msg):
        self.test_log.configure(state="normal")
        self.test_log.insert("end", msg + "\n")
        self.test_log.see("end")
        self.test_log.configure(state="disabled")

    def _start_test(self, which):
        if self._test_running:
            return
        if not self.mk:
            self._tlog("ERROR: not connected — connect on the Device tab first.")
            return
        try:
            duration = float(self.test_dur.get())
            if not 0 < duration <= 60:
                raise ValueError
        except ValueError:
            self._tlog("ERROR: duration must be a number between 0 and 60 seconds.")
            return
        self._test_running = True
        self.test_stop_evt.clear()
        self.test1_btn.config(state="disabled")
        self.test2_btn.config(state="disabled")
        self.test_stop_btn.config(state="normal")
        name = "LB/RB spam" if which == 1 else "aim-stick sweep"
        self._tlog(f"Test {which} ({name}) started — {duration:g}s. "
                   "Wiggle a stick if nothing shows on the console.")
        worker = self._test1_worker if which == 1 else self._test2_worker
        threading.Thread(target=worker, args=(duration,), daemon=True).start()

    def _stop_test(self):
        self.test_stop_evt.set()

    def _test1_worker(self, duration):
        """Press LB+RB for 0.6 s, release for 0.4 s — long enough for the
        console to register each press (short taps can fall between reports)."""
        sent = errors = 0
        end = time.monotonic() + duration
        pressed = False
        while time.monotonic() < end and not self.test_stop_evt.is_set():
            pressed = not pressed
            try:
                with self.io_lock:
                    if pressed:
                        self.mk.hold("LB")
                        self.mk.hold("RB")
                    else:
                        self.mk.release("LB")
                        self.mk.release("RB")
                sent += 2
            except Exception as e:
                errors += 1
                self.root.after(0, self._tlog, f"ERROR sending: {e}")
                if errors >= 5:
                    self.root.after(0, self._tlog, "Too many errors — aborting.")
                    break
            time.sleep(0.6 if pressed else 0.4)
        try:
            with self.io_lock:
                self.mk.release("LB")
                self.mk.release("RB")
        except Exception as e:
            errors += 1
            self.root.after(0, self._tlog, f"ERROR on final release: {e}")
        self.root.after(0, self._test_done, "LB/RB released", sent, errors,
                        self.test_stop_evt.is_set())

    def _test2_worker(self, duration):
        """Slam the aim (right) stick full left / full right via km.trim."""
        sent = errors = 0
        end = time.monotonic() + duration
        left = True
        while time.monotonic() < end and not self.test_stop_evt.is_set():
            try:
                with self.io_lock:
                    self.mk.trim(-32767 if left else 32767, 0)
                sent += 1
            except Exception as e:
                errors += 1
                self.root.after(0, self._tlog, f"ERROR sending: {e}")
                if errors >= 5:
                    self.root.after(0, self._tlog, "Too many errors — aborting.")
                    break
            left = not left
            time.sleep(0.4)
        try:
            with self.io_lock:
                self.mk.trim(0, 0)
        except Exception as e:
            errors += 1
            self.root.after(0, self._tlog, f"ERROR clearing trim: {e}")
        self.root.after(0, self._test_done, "trim cleared", sent, errors,
                        self.test_stop_evt.is_set())

    def _test_done(self, cleanup, sent, errors, stopped):
        self._test_running = False
        self.test1_btn.config(state="normal")
        self.test2_btn.config(state="normal")
        self.test_stop_btn.config(state="disabled")
        outcome = "STOPPED by user" if stopped else "completed"
        self._tlog(f"Test {outcome} — {sent} commands sent, {errors} error(s). "
                   f"{cleanup}.")
        self.status.set(f"Test {outcome}: {sent} commands, {errors} errors.")

    # -------------------------------------------------------------- Telemetry IO
    def _start_reader(self):
        self.reader_stop.clear()
        self.reader = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader.start()

    def _stop_reader(self):
        self.reader_stop.set()
        self.reader = None

    def _reader_loop(self):
        buf = b""
        while not self.reader_stop.is_set() and self.mk:
            try:
                buf += self.mk.ser.read(256)
            except Exception:
                break
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                s = line.decode("ascii", "replace").strip()
                if not s.startswith("KMS "):
                    continue
                d = {}
                for tok in s[4:].split():
                    k, _, val = tok.partition("=")
                    try:
                        d[k] = int(val, 16) if k == "b" else int(val)
                    except ValueError:
                        pass
                if {"lx", "ly", "rx", "ry"} <= d.keys():
                    self.tel_q.put(d)

    def _drain_telemetry(self):
        last = None
        try:
            while True:
                d = self.tel_q.get_nowait()
                last = d
                if self._measuring:
                    self._measure_samples.append(d)
        except queue.Empty:
            pass
        if last:
            for k in ("lx", "ly", "rx", "ry"):
                self.mon_vars[k].set(str(last.get(k, "—")))
            b = last.get("b", 0)
            self.mon_vars["b"].set(f"{b:#06x}")
            for name, lbl in self.mon_btn_labels.items():
                m = self.btn_map[name]
                if (b & m) == m:
                    lbl.config(bg="#2a6", fg="white")
                else:
                    lbl.config(bg="#e8e8e8", fg="#888")
        self.root.after(60, self._drain_telemetry)

    def on_close(self):
        self.disconnect()
        self.root.destroy()


def main():
    root = tk.Tk()
    try:
        ttk.Style().theme_use("clam")
    except Exception:
        pass
    app = MakcuGUI(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()


if __name__ == "__main__":
    main()
