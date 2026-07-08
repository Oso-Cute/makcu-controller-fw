# MAKCM Controller Passthrough — Issue & Solution Report

**Date:** 2026-07-07
**Goal:** Use a MAKCM (dual-ESP32-S3 MAKCU) device to pass a game controller
through to an Xbox, then add **accessibility** features (tremor damping, drift
trim, hold-to-toggle buttons, foot-pedal support).

---

## 1. TL;DR

The controller passthrough did **nothing** — no injection, no stick data, no
button remap reached the Xbox. After deep diagnosis this turned out to be
**three stacked problems**, all on top of a base firmware the original author
publicly called *unfinished*:

1. **Only one of the two MCUs was flashed** with the matching firmware.
2. **The Right MCU needs a hard power-cycle** after flashing or it stays silent.
3. **The firmware never generated the GIP handshake**, so Xbox One (GIP)
   controllers stayed stuck "announcing" forever and never sent input.

Fixing all three — flash **both** chips, power-cycle, and **implement the GIP
init sequence** — made the full pipeline work: the controller now streams input,
our `km_apply` injection layer runs (verified 22,851 calls), and full stick +
button data is live. The accessibility layer (steady/trim/toggle/monitor + a new
GUI) now has real data to act on.

---

## 2. Background / what the hardware is

- **MAKCM** = two ESP32-S3 chips on one board, 3 USB ports:
  - **USB1 (left)** → Left MCU = USB **device** to the PC/console (the Xbox).
    Also powers the Left MCU.
  - **USB2 (middle)** → **CH343** USB-serial bridge = the **command port**
    (`km.*` ASCII API, 4 Mbaud). Shows up as a COM port (e.g. **COM5**), VID
    `1A86`. This is the Left MCU's UART0.
  - **USB3 (right)** → Right MCU = USB **host** for the real controller.
- **Left MCU firmware** (`MAKCM_ESP32s3_Pass_Left_IDF`, ESP-IDF + TinyUSB):
  presents as the controller to the Xbox, owns the `km_inject` pipeline and the
  KM command UART.
- **Right MCU firmware** (`MAKCM_ESP32s3_Pass_Right`, Arduino + ESP-IDF
  usb_host): enumerates the real controller, relays descriptors + forwards
  every USB transfer to the Left over a 5 Mbps internal UART (IPC).
- The base firmware (`MAKCM_Pass_Package`) was published by the MAKCU dev as
  **unfinished example code** ("not a fully finished product… no expectation to
  fix, finish, support… you are on your own"). Community reports matched:
  *"only one light works," "it wasn't a real controller firmware."*

Our accessibility additions (steady filter, trim, telemetry, hold-toggle) live
inside the Left's `km_inject.c` and are driven over the CH343 command port.

---

## 3. The symptom

Everything **looked** flashed and alive:
- `km.version()` on COM5 returned `kmbox: 1.0.0 <our build timestamp>` — proving
  our Left firmware was running.
- New `km.steady` / `km.trim` / `km.telem` commands were accepted.
- The controller appeared to "work" on the Xbox.

But nothing our firmware did reached the Xbox, and telemetry showed **all
zeros**. A diagnostic probe (added counter `n` = number of `km_apply` calls,
plus last endpoint / first byte / length) proved the smoking gun:

```
n=0  ep=00  b0=00  len=0
```

**`km_apply` was never being called.** The report-processing pipeline our
accessibility layer hooks into was completely idle — no controller input was
flowing through it.

---

## 4. Diagnostic journey (and the false trails)

1. **Port mapping confusion.** The 3 USB ports were mislabeled by guesswork.
   Resolved by plugging one at a time + reading VID/PID, and later confirmed
   against the official docs (github.com/tacorwin/makcm, makcu.com): USB1=console
   device, USB2=CH343 command, USB3=controller host.

2. **Wrong chip / port theories.** Spent effort deciding which physical chip
   served the Xbox. The Left's own USB presents as a *controller* (TinyUSB), not
   a serial port, so it showed no COM — which looked like "dead port" but was
   normal.

3. **Turned the firmware's own logging back on.** The quiet gameplay build
   (`COM3_LOG=0`) silences the extensive `[L]`/`[R]` diagnostics. Rebuilding with
   `COM3_LOG=1` (but `KM_DIAG/KM_RING/LAT_DIAG=0` to stay readable) exposed the
   real internal state on COM5. **This was the turning point** — it let us watch
   the Right→Left handshake and the USB traffic directly.

4. **Discovered the Right MCU was sending nothing.** With logging on, the Left
   received **zero** `[R]` frames. Root: the Right chip was still running the
   **pre-flashed vendor (mouse) firmware**, which speaks a different IPC
   protocol than our Left. → **Problem #1: only the Left was flashed.**

5. **Flashed the Right too — still silent.** After flashing the Right it didn't
   come alive until a **full power-cycle** (unplug all, wait, replug). →
   **Problem #2: Right needs a hard reset after flashing.**

6. **Pipeline came alive, but still no input.** After both fixes the logs showed
   the full handshake: `[R] NEW_DEV`, descriptor relay, `[L] start_usb`, the
   Xbox resetting our device, and **`EP_OUT`** (Xbox→controller) flowing. But
   `EP_IN` (controller→Left) was only the **GIP "announce"** packet (`02 20 …`),
   repeating every ~500 ms forever. The controller never graduated to input
   reports. → **Problem #3: no GIP init.**

7. **Red herring: a "working" wireless pad.** Early on the controller *did*
   navigate the Xbox — because it was an Xbox One pad connected **wirelessly** to
   the console at the same time it was cabled to the MAKCU. The MAKCU wasn't the
   path at all. Switching to a **wired-only** controller removed this illusion
   and confirmed the wired passthrough delivered no input.

---

## 5. Root cause

Xbox One / GIP controllers do not stream input until the **host** completes the
**GIP (Game Input Protocol) initialization handshake**: the device sends an
"announce," and the host must reply with identify / power-on / LED commands. The
Right MCU firmware **enumerated** the controller and opened its endpoints but
**never generated any GIP packets** (the header literally said *"No GIP
handshake generation — passes the wire data through unchanged"*). So the
controller sat in its announce loop forever, no input was produced, and there
was nothing for `km_apply` (or the Xbox) to receive.

---

## 6. The solution

### 6.1 Flash BOTH chips with the matching package firmware
- Left: `MAKCM_ESP32s3_Pass_Left_IDF` (ours, with the accessibility additions).
- Right: `MAKCM_ESP32s3_Pass_Right`.
- They share a custom IPC protocol — a mismatched Right breaks the whole chain.

### 6.2 Power-cycle after flashing the Right
- esptool over USB-Serial/JTAG often can't reset the chip; a manual full unplug/
  replug is required for the new firmware to actually run.

### 6.3 Implement the GIP init handshake on the Right (the real fix)
Added to `PassUsbHost.cpp` (+ `PassUsbHost.h`):

- Learn the interrupt **OUT** endpoint from the descriptors (`gip_out_ep_`).
- On receiving the **announce** (`in_xfer_complete`, first byte `0x02`) while no
  input has been seen yet, send the GIP init sequence to the controller:

  | Step | Command | Bytes on wire |
  |------|---------|---------------|
  | Identify | `0x04` | `04 20 <seq> 00` |
  | Power on | `0x05` | `05 20 <seq> 01 00` (mode `0x00` = on) |
  | Set LED | `0x0a` | `0a 20 <seq> 03 00 01 14` |

  GIP header = `[command][options][sequence][length][payload]`; options =
  `client_id(0) | GIP_OPT_INTERNAL(0x20)`; sequence is non-zero and increments;
  packets padded to even length. Sequence source: the Linux **xone** driver and
  TheNathannator's GIP protocol notes.

- Once a real input report (`0x20`) is seen, stop re-kicking (`gip_got_input_`).

The controller then transitions **announce → identify → input reports (`0x20`)**,
which are exactly what `km_apply` processes (EP `0x82`, `buf[0]==0x20`).

---

## 7. Verification (on hardware)

After the fix, a live telemetry capture while moving the sticks:

```
n (km_apply calls) = 22,851        (was 0)
ep = 0x82   b0 = 0x20   len = 36    (GIP input reports)
rx: -32768 … +32767   (full right-stick X range)
lx / ly / ry: full range
b: button bit 0x4000 observed
```

Input histogram over ~25 s: `{0x20: 159 input, 0x02: 27 announce, 0x04: 6
identify, …}` — the controller graduated from announce to input.

---

## 8. What we had to overcome (obstacles list)

- **Undocumented, admittedly-unfinished base firmware** — no support, no spec.
- **Ambiguous 3-port hardware** — resolved by empirical VID/PID probing + docs.
- **Silenced diagnostics** — had to rebuild with `COM3_LOG=1` to see anything;
  the log build floods COM5 and interleaves with telemetry (readability tradeoff).
- **Two-chip firmware mismatch** — non-obvious that BOTH must be flashed.
- **Flash quirks:**
  - Left flashes over its native USB-Serial/JTAG (`303A:0009`) — hold the USB1
    boot button while plugging into the PC → COM appears → flash. esptool then
    reports `Error 1` "can not exit download mode over USB" *after* a verified
    write — **this is success**, just power-cycle.
  - Right flashes over its native USB (`303A:1001`) directly, no button.
  - CH343/UART0 auto-reset flashing does **not** work on this board.
  - Right must be **power-cycled** after flashing.
- **Wireless-controller red herring** — a paired Xbox pad masked the failure;
  needed a wired-only controller to get a clean signal.
- **GIP protocol knowledge** — had to pull the exact init byte sequences from the
  xone driver rather than guess.

---

## 9. Current state

- Full wired controller → MAKCU → Xbox passthrough works, with our injection
  layer in the path.
- Accessibility features available over the CH343 command port (COM5):
  - `km.steady` / `km.steady_a` / `km.steady_d` — tremor low-pass + deadzone
  - `km.trim(x,y)` — drift cancel / gentle constant pull
  - `km.telem` — physical stick + button telemetry (drives the monitor/GUI)
  - hold/toggle button latches (`km.btnA(1)` … one-handed use)
- Tooling in `accessibility/`: `makcu_gui.py` (tabbed panel with a device-detect
  tab), `makcu_monitor.py` (tremor measurement), `makcu_access.py` (wrapper +
  foot-pedal example).

## 10. Remaining caveats / future work

- **GIP init is minimal** (identify + power-on + LED). Some controllers may need
  acknowledgement (ACK, cmd `0x01`) or descriptor-request handling — add if a
  given pad still won't stream.
- **Steady/trim currently affect the right (aim) stick only**; left (movement)
  stick is a documented follow-up.
- The **log build** floods COM5 — reflash the quiet Left build (`COM3_LOG=0`,
  the source default) for cleanest telemetry.
- **Console auth**: the wired GIP pad answers its own handshake through the
  passthrough; not all third-party pads are guaranteed to.
- Diagnostic logging (`[L] EP_IN/EP_OUT`, `[R] IN done`, `GIP send`, `km_apply`
  probe counter) is still in the firmware — harmless, can be trimmed for a
  release build.
