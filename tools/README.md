# tools/

Small helper utilities that aren't part of day-to-day use.

| File | What |
|------|------|
| `button_mapper.py` | Interactive logger: press each controller button when prompted, it captures which bit lights up in the telemetry `b=` mask and writes `button_map.json`. The GUI Monitor tab reads that file to show button names (e.g. `LB + RB`) instead of raw hex. Run once per controller model. |
| `button_map.json` | Output of the mapper (created on first run, one per repo checkout — remap if you switch controller models). |

```
python tools/button_mapper.py          # auto-detects the CH343 port
python tools/button_mapper.py COM5     # or name it explicitly
```

Notes:
- Triggers and sticks are analog — they don't appear in the `b` bitmask
  (sticks are the `lx/ly/rx/ry` fields; triggers aren't in telemetry yet).
- Mapping is per controller family (GIP pads generally share one layout).
