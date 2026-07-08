"""
makcu_monitor.py — measure your controller shake, then recommend steady-filter
settings tuned to it.

Requires the firmware built with the telemetry support (km.telem) and flashed
to the Left MCU. This script:
  1. turns telemetry on,
  2. shows live stick values + which buttons are pressed,
  3. samples the RIGHT (aim) stick while you HOLD it as still as you can,
  4. reports the shake amplitude and suggests km.steady_d / km.steady_a values.

Run it, hold the stick steady (or just rest your hand on the controller the way
you normally would), let it collect ~5 seconds, and read the recommendation.

    python makcu_monitor.py

Requires: pip install pyserial
"""

import time
import statistics

from makcu_access import Makcu, load_config


# Raw stick range is roughly -32768..32767. Button bit -> readable name is
# controller-specific; we just show the raw hex so you can see *that* a button
# fired and which bit.
def summarize(samples, axis):
    vals = [s[axis] for s in samples]
    if not vals:
        return None
    return {
        "min": min(vals),
        "max": max(vals),
        "p2p": max(vals) - min(vals),          # peak-to-peak = shake width
        "stdev": statistics.pstdev(vals),      # spread around the mean
        "mean": statistics.fmean(vals),
    }


def main():
    cfg = load_config()
    mk = Makcu(cfg["port"])
    print("link:", mk.version().strip())
    mk.telem(True)
    print("telemetry ON. Hold the RIGHT stick as steady as you can.")
    print("Collecting 5 s ...  (buttons you press will show as changing b=)")

    samples = []
    seen_btn = set()
    t_end = time.time() + 5.0
    last_print = 0
    for d in mk.read_telem():
        samples.append(d)
        if d["b"]:
            seen_btn.add(d["b"])
        now = time.time()
        if now - last_print > 0.25:
            last_print = now
            print(f"\r rx={d['rx']:+6d} ry={d['ry']:+6d}  "
                  f"lx={d['lx']:+6d} ly={d['ly']:+6d}  b={d['b']:#06x}",
                  end="", flush=True)
        if now >= t_end:
            break
    print("\n")

    rx = summarize(samples, "rx")
    ry = summarize(samples, "ry")
    if not rx:
        print("no telemetry received — is the firmware telem build flashed, "
              "and the controller a supported type (GIP/XInput/DS)?")
        return

    print(f"right stick X: peak-to-peak={rx['p2p']}  stdev={rx['stdev']:.0f}")
    print(f"right stick Y: peak-to-peak={ry['p2p']}  stdev={ry['stdev']:.0f}")
    if seen_btn:
        print("buttons seen (raw bits):", ", ".join(hex(b) for b in sorted(seen_btn)))

    # Recommendation. Deadzone should swallow the tremor: set it a bit above the
    # larger axis's peak-to-peak so idle shake reads as center. Smoothing scales
    # with how jittery it is.
    shake = max(rx["p2p"], ry["p2p"])
    dead = min(32000, int(shake * 0.6))          # ~60% of p2p as radial deadzone
    alpha = 60 if shake < 4000 else 75 if shake < 12000 else 85
    print("\nSuggested starting point:")
    print(f"    mk.steady_deadzone({dead})")
    print(f"    mk.steady_smoothing({alpha})")
    print(f"    mk.steady(True)")
    print("Then feel it out in-game and nudge from there — bigger deadzone if")
    print("aim still drifts, lower smoothing if it feels laggy.")

    # Drift trim: if the stick rests off-center (hardware drift), the mean is
    # nonzero. Cancel it with the opposite offset. Only bother if it's sizable.
    tx, ty = round(rx["mean"]), round(ry["mean"])
    if abs(tx) > 1500 or abs(ty) > 1500:
        print(f"\nResting off-center by ({tx:+d},{ty:+d}) — looks like drift.")
        print(f"Cancel it with:  mk.trim({-tx},{-ty})")

    mk.telem(False)


if __name__ == "__main__":
    main()
