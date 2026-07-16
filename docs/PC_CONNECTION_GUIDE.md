# Connecting Xbox Controllers to a PC (v0.5.0)

How to use the MAKCU passthrough to bring a wired Xbox controller into a
**PC** — verified with the Xbox Elite (Series 1, model 1698), Xbox One S
(1708), and Xbox Series X (1914) controllers.

The PC sees a mirrored **"Xbox Controller" (VID 045E, PID 0B12)** and uses
it like any wired Xbox pad. The PC is much more forgiving than an Xbox
console — it enumerates the controller in seconds and does not run the
strict authentication loop, so there is no long dark-light wait on PC.

---

## What you need

- MAKCU board flashed with **v0.5.0** on **both** MCUs (see the
  [release](https://github.com/Oso-Cute/makcu-controller-fw/releases)).
  Both sides must match — Left-only or Right-only will not work.
- A wired Xbox controller (1698 / 1708 / 1914).
- A PC with at least two free USB ports.

One PC can play both roles: the "console" connection (USB1) and the
diagnostics connection (USB2) just go to two different ports on the same
PC.

---

## Port map

| Makcu port | Goes to | Purpose |
|---|---|---|
| **USB3** (right) | the controller | the real controller plugs in here |
| **USB2** (middle) | a PC USB port | command/diagnostics (CH343 COM port) — optional |
| **USB1** (left)  | a PC USB port | the "host" — **this powers the board** |

The board has no power until **USB1** is connected. If you plug in USB2
and the board looks dead, that is normal — it wakes when USB1 goes in.

---

## Steps

1. **Start with everything unplugged.**
2. Plug the **controller into USB3**.
3. Plug **USB2 into the PC** (optional — only needed for the accessibility
   tools / diagnostics).
4. Plug **USB1 into the PC last.** The board powers up and the PC begins
   enumerating the mirrored controller.
5. Wait a few seconds. On PC this is quick — no long wait.
6. **Verify:** press `Win + R`, run `joy.cpl`. You should see
   **"Controller (Xbox …)"** / "Xbox Controller". Open it and move the
   sticks / press buttons — they should register in the test panel.

That's it. The controller now works in any game or app that accepts an
Xbox controller.

---

## Tips

- **Wake the controller on a console first (once).** If a controller is
  slow to come up, plug it directly into an Xbox for a moment so it powers
  on and starts announcing cleanly, then move it back to the Makcu's USB3.
  This helps the older 1708 / 1698 especially.
- **All three models use one firmware.** Nothing to change per controller
  — the passthrough copies each controller's identity automatically.
- **Elite (1698) note:** the Elite streams its input on a different USB
  endpoint (0x81) than the 1708 / 1914 (0x82). v0.5.0 handles both, so the
  Elite gets full aim-injection support, not just plain passthrough.
- **Use a plain wired controller.** Everything here assumes a standard
  wired GIP controller.
- **Accessibility / mouse aim** (the `km.*` command layer, steady filter,
  trim, button latches, spam/kill test tabs) runs over the **USB2** CH343
  command port — see [`accessibility/README.md`](../accessibility/README.md).

---

## If it doesn't show up

- **Nothing in `joy.cpl`:** confirm **USB1 is in a PC port** (the board is
  dead without it) and the controller is firmly in **USB3**. Try a full
  cold start — unplug everything, wait ~10 s, redo the order above.
- **Board seems unresponsive after flashing:** the Right MCU needs a full
  power-cycle after a flash before it runs. Unplug all cables, wait, replug.
- **Want to see what's happening:** connect USB2, open the command port at
  **4,000,000 baud** (it's a CH343, VID 1A86), or run
  `python accessibility/makcu_access.py` for a `kmbox:` handshake line.
- **One specific controller won't inject aim but works as a controller:**
  that means its report is passing through but the injection is skipped —
  capture its report and open an issue; that is exactly how the Elite
  (1698) fix was found.
