# tools/

Small helper utilities that aren't part of day-to-day use.

| File | What |
|------|------|
| `makcm_debug.py` | **"My controller gives no input" debugger.** Guided capture: enables the firmware's runtime diagnostic stream (`km.debug(1)`), has you replug the controller, then tells you WHERE the pipeline breaks (controller → Right MCU → IPC → Left MCU → console) and saves a `.log` to attach to a GitHub issue. Needs firmware built 2026-07-10 or later on the Left MCU. |
| `button_mapper.py` | Interactive logger: press each controller button when prompted, it captures which bit lights up in the telemetry `b=` mask and writes `button_map.json`. The GUI Monitor tab reads that file to show button names (e.g. `LB + RB`) instead of raw hex. Run once per controller model. |
| `button_map.json` | Output of the mapper (created on first run, one per repo checkout — remap if you switch controller models). |

```
python tools/makcm_debug.py            # auto-detects the CH343 port
python tools/makcm_debug.py --port COM5 --seconds 30
```
`run_makcm_debug.bat` is a Windows double-click wrapper for the same tool
(installs pyserial if missing, keeps the window open to read the verdict).
Wiring while debugging: PC → USB2 (middle) **and** USB1 plugged in (it powers
the Left MCU); controller → USB3.

```
python tools/button_mapper.py          # auto-detects the CH343 port
python tools/button_mapper.py COM5     # or name it explicitly
```

Notes:
- Triggers and sticks are analog — they don't appear in the `b` bitmask
  (sticks are the `lx/ly/rx/ry` fields; triggers aren't in telemetry yet).
- Mapping is per controller family (GIP pads generally share one layout).
