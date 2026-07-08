# Pre-publish hardware validation checklist (Right firmware rebuild)

Run this after any Right MCU rebuild, before pushing the repo public.

1. **Flash** `firmware/bin/MERGED_right.bin` to the Right MCU
   (plug USB3 into the PC, no boot button needed):
   ```
   esptool.py --chip esp32s3 --port <COM> --baud 921600 write_flash 0x0 firmware/bin/MERGED_right.bin
   ```
2. **Power-cycle the board** — unplug all cables, wait a few seconds, replug.
   The Right MCU does not run new firmware without this.
3. **Confirm telemetry streams:** reconnect for normal use (PC → USB2,
   console → USB1, controller → USB3), then
   `python accessibility/makcu_monitor.py` — expect live `KMS …` lines that
   change when you move the sticks.
4. **Open the Test tab:** `python accessibility/makcu_gui.py` → connect on
   the Device tab → switch to **Test**.
5. **Run both tests** — leave duration at 4 seconds; **Test 1** (LB/RB spam),
   then **Test 2** (aim-stick sweep). Wiggle a stick slightly during each —
   GIP pads only emit reports on change and injection rides on reports.
6. **Confirm inputs register** on the console/PC (LB/RB flicker in the
   game's input display or a gamepad tester).
7. **Confirm no errors** in the Test tab log — expect
   `Test completed — N button commands sent, 0 error(s).`
8. **String / history check (if the binary changed):**
   - no personal paths in the bin: scan `MERGED_right.bin` for `Users`,
     your username, etc.
   - no leaky blobs in history: scan every `firmware/bin/MERGED_*` blob in
     `git rev-list main`.
9. **Only then push public.**

If any step fails, stop — fix and restart from step 1.
