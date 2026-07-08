# MAKCM accessibility control layer

Non-cheat use of the MAKCM passthrough firmware. Your real controller drives
the game normally through the passthrough; this Python layer only **adds
sustained button holds** on top, triggered by an accessibility input (foot
pedal, switch, or keyboard key). The point: turn a **momentary tap into a
held button** so you never have to physically hold anything.

## What's here

| File | Purpose |
|------|---------|
| `makcu_gui.py` | **Tabbed GUI** — device detection, steady, trim, live monitor, button latches |
| `makcu_access.py` | `Makcu` serial wrapper + a runnable foot-pedal example |
| `makcu_monitor.py` | CLI tremor measurement + recommended settings |

## GUI (easiest way to use it)

```
pip install pyserial
python makcu_gui.py
```

Tabs:
- **Device** — lists serial ports and flags the CH343 (the command port). Click
  *Auto-detect CH343* → *Connect* → *Handshake test* (expects `kmbox: 1.0.0…`).
- **Steady** — enable the tremor filter; sliders for smoothing + deadzone (live).
- **Trim** — constant stick offset to cancel drift.
- **Monitor** — live stick/button readout; *Measure shake (5s)* auto-fills the
  recommended steady + trim values.
- **Buttons** — latch toggles: tap once = button held, tap again = release.
- **Test** — manual hardware validation: spams LB/RB for a few seconds so you
  can confirm inputs reach the console/PC. Only runs when you click Start;
  Stop button interrupts immediately.

> The command port only responds when USB1 (Left MCU) is powered and USB2
> (CH343) is in this PC. For the controller pipeline to feed real data, both
> MCUs must run the package firmware — see the repo `FLASHING.md`.

## Requirements

```
pip install pyserial pynput
```

- **pyserial** — talks to the firmware over the CH343 COM port.
- **pynput** — reads pedal/key presses on this PC.

## One-time setup

1. Flash the **quiet** firmware build to the Left MCU (the log build floods the
   COM port and will fight this script).
2. Find the CH343's COM port (Device Manager → Ports). Set `"port"` in
   `config.json` (auto-created next to the scripts on first run).
3. Run it: `python makcu_access.py`. It prints the `kmbox: 1.0.0 ...` handshake
   line — if you see that, the link works.

## Foot pedal

The common USB foot pedals (PCsensor FS2020 and clones) act as a **keyboard** —
each pedal sends a keystroke. Recommended setup:

1. Open the pedal's config tool.
2. Set each pedal to send an **unused key**: `F13`, `F14`, `F15` … (nothing else
   on Windows uses F13–F24, so no conflicts).
3. Map those keys in `config.json` under `"pedal_map"`:

   ```json
   {
     "port": "COM3",
     "pedal_map": {
       "f13": "RT",
       "f14": "A",
       "f15": "LT"
     }
   }
   ```
   (left pedal → right trigger/fire, middle → A, right → left trigger/ADS —
   all latched.)

Tap a pedal once → that button latches **held**. Tap again → released. `Esc`
releases everything and quits.

### If your pedal is NOT a keyboard

Some pedals present as a HID game device instead. Two options:
- Use the pedal's software to switch it to **keyboard mode** (most can), or
- Read it as a gamepad with `pip install inputs` and call `mk.toggle("A")`
  from that event loop instead — same `Makcu` object, different trigger source.

## Buttons you can drive

Confirmed from the firmware (`km_inject.c`). Only these exist:

| Name in code | Controller button |
|--------------|-------------------|
| `A` `B` `X` `Y` | face buttons |
| `LB` `RB`       | bumpers |
| `RT`            | right trigger / "fire" |
| `LT`            | left trigger / "ADS" |

Movement is delta-only (`mk.move(dx, dy)`) — no absolute aim, no smoothing.
Those are stick-aim features and are not needed for button-hold accessibility.

## Controller compatibility

The firmware only rewrites reports for **Xbox One/GIP, XInput (Xbox 360), and
DualShock 4/5**. If your controller is one of those, latched holds work. Any
other controller still passes through and plays normally, but the added holds
won't appear.

## Tremor / shake damping ("steady" filter)

If your controller shakes (e.g. it sits on a leg that moves), Python **cannot**
fix that alone — the firmware passes your real stick straight through. Two real
fixes:

1. **Mount the controller** to a fixed surface first. Removes the shake at its
   source and takes weight off your hand. Try this before anything else.
2. **The firmware steady filter** (added to `km_inject.c`): a low-pass +
   deadzone on the **right / aim stick**, tunable live over the KM UART.

> **This needs the modified firmware flashed to the Left MCU** (the code is in
> `firmware/MAKCM_ESP32s3_Pass_Left_IDF/src/km_inject.c`; the prebuilt images
> in `firmware/bin/` include it).

### Measure your shake first

`makcu_monitor.py` reads telemetry the firmware streams and tells you how big
your tremor actually is, then suggests filter values:

```
python makcu_monitor.py
```

Hold the stick as steady as you can for 5 s; it prints peak-to-peak shake and a
recommended `steady_deadzone` / `steady_smoothing`.

### Apply and tune (live, no reflash)

```python
from makcu_access import Makcu
mk = Makcu("COM3")
mk.steady_deadzone(6000)   # shake below this reads as center
mk.steady_smoothing(75)    # 0..99, higher = smoother but more lag
mk.steady(True)            # turn it on
```

- Aim still drifts → raise the deadzone.
- Feels laggy/floaty → lower the smoothing.
- `mk.steady(False)` to turn off.

### New firmware commands (raw)

| Command | Effect |
|---------|--------|
| `km.steady(1\|0)`  | enable/disable the aim-stick filter |
| `km.steady_a(N)`   | smoothing 0..99 (EMA weight of history) |
| `km.steady_d(N)`   | deadzone 0..32000 |
| `km.telem(1\|0)`   | stream `KMS lx= ly= rx= ry= b=` lines for the monitor |
| `km.trim(x,y)`     | constant right-stick offset — cancel drift or add a gentle pull |

### Stick drift / constant pull (`km.trim`)

For a stick that rests off-center (hardware drift), or if you *want* a gentle
constant pull, use a fixed offset — **not** repeated `km.move` (that decays and
runs through the aim curve). `makcu_monitor.py` reports resting off-center and
prints the exact `mk.trim(...)` to cancel it. Manually:

```python
mk.trim(0, -6000)   # cancel a downward drift of ~6000, or add an upward pull
mk.trim(0, 0)       # clear
```

Caveat: the firmware already ignores drift below its built-in ~4000 deadzone, so
trim matters for drift big enough to actually move your view. Trim is applied
before the steady filter, so the two stack cleanly.

### Scope / limitation

The filter currently damps the **right (aim) stick only** — that's what the
firmware's injection pipeline already handles. The **left (movement) stick** is
still passed through raw. If the monitor shows your left stick shaking enough to
matter too, extending the filter to it is a natural follow-up (same technique,
a few more lines in each `apply_*`) — contributions welcome.

## Console note

The real controller stays plugged into the Right MCU and answers the console's
auth handshake itself, so an Xbox console *may* accept this — untested on this
firmware, so verify before relying on it. PC works without that concern.
