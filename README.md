# MAKCM Controller Passthrough — community fix

This was 100% done with the help of Claude.

💬 **Community:** [Join the Discord](https://discord.gg/hPFZbJwY2Z) — come share
what you're building with Titan2 and makcu/controller.

## Demo video

[![MAKCU Firmware Tools — Quick Intro](https://img.youtube.com/vi/0g_Uu8EqmU4/hqdefault.jpg)](https://youtu.be/0g_Uu8EqmU4)

Quick walkthrough of the repo layout, example firmware/tools, and hardware
validation flow.

The MAKCM (dual-ESP32-S3 "MAKCU" board) **game controller passthrough
firmware** shipped unfinished. This repo carries the fixes needed to make it
work end-to-end, plus a small Python GUI for quick testing.

The device sits between a real controller and the target console/PC, passes
the controller through transparently, and exposes a serial command channel
that the Python tools use to add assistance.

> **Not an official MAKCU project.** This is based on the
> `MAKCM_Pass_Package` example firmware released by the MAKCM/MAKCU author,
> which was published explicitly as unfinished example code. See
> [docs/legal/THIRD_PARTY_NOTICES.md](docs/legal/THIRD_PARTY_NOTICES.md)
> for credits and licensing status.

## What was fixed

The stock package firmware did not deliver any controller input. Getting it
working required three fixes (full write-up in
[docs/ISSUE_REPORT.md](docs/ISSUE_REPORT.md)):

1. **Both MCUs must be flashed** with the matching firmware pair — they share
   a custom IPC protocol, and a board fresh from the vendor is running
   different stock firmware on the Right MCU.
2. **The Right MCU needs a full power-cycle after flashing** or it stays
   silent.
3. **GIP init handshake implemented** on the Right MCU — Xbox One (GIP)
   controllers never leave their "announce" loop without it, so no input ever
   flows. This is the main firmware change in this repo.

On top of that, the Left MCU gained accessibility features: a steady
(anti-tremor) filter, stick-drift trim, live telemetry, and button hold/toggle
latches, all controllable over the serial command port.

## Layout

| Path | What |
|------|------|
| `firmware/` | The two PlatformIO projects (Left = ESP-IDF, Right = Arduino + ESP-IDF usb_host), a guided flash tool, and prebuilt merged images in `firmware/bin/`. |
| `accessibility/` | Python control layer: tabbed GUI, tremor monitor, serial wrapper + foot-pedal example. Start here for day-to-day use. |
| `tools/` | Extras — `button_mapper.py` logs which telemetry bit each controller button sets, so the GUI Monitor can show button names. |
| `docs/` | [ISSUE_REPORT.md](docs/ISSUE_REPORT.md) — the debugging write-up; [UPSTREAM_PACKAGE_README.md](docs/UPSTREAM_PACKAGE_README.md) — the original package README, kept as the technical reference for the passthrough internals. |
| `FLASHING.md` | How to flash **both** MCUs, step by step. |

## Requirements

- **Hardware:** a MAKCM/MAKCU board (two ESP32-S3, 4 MB flash, three USB
  ports), a wired controller, and a PC for flashing and the Python tools.
  Tested on one setup: MAKCM board + wired Xbox One (GIP) controller + Xbox
  console + Windows 11 PC. Other combinations should work per the upstream
  docs but are unverified here.
- **To flash prebuilt images:** Python 3 + `pip install pyserial esptool`.
- **To build from source:** [PlatformIO](https://platformio.org/) CLI
  (`pip install platformio` or the VS Code extension). The ESP-IDF toolchain
  is downloaded automatically on first build.
- **For the accessibility tools:** `pip install pyserial pynput`.

## Quick start

1. **Flash both MCUs.** Easiest: `python firmware/flash_tool.py` (guided GUI,
   uses the prebuilt images in `firmware/bin/`). Or follow
   [FLASHING.md](FLASHING.md) manually. **Both sides must be flashed** —
   Left-only or Right-only will not work.
2. **Power-cycle everything** (unplug all cables, wait a few seconds, replug).
   The Right MCU will not run new firmware until this is done.
3. **Connect for use:** PC → middle port (USB2, the CH343 command port),
   console/PC → left port (USB1), controller → right port (USB3).
4. **Verify:** `python accessibility/makcu_access.py` — expect a
   `kmbox: 1.0.0 …` handshake line.
5. **Use the GUI:** `python accessibility/makcu_gui.py` — see
   [accessibility/README.md](accessibility/README.md).

## Build from source

```bash
# Left MCU — quiet build (use this for play):
PLATFORMIO_BUILD_FLAGS="-DCOM3_LOG=0 -DKM_DIAG=0 -DKM_RING=0 -DLAT_DIAG=0" \
  pio run -d firmware/MAKCM_ESP32s3_Pass_Left_IDF -e LEFT_IDF

# Left MCU — log build (diagnostics only; floods the command port):
pio run -d firmware/MAKCM_ESP32s3_Pass_Left_IDF -e LEFT_IDF

# Right MCU:
pio run -d firmware/MAKCM_ESP32s3_Pass_Right -e RIGHT
```

Flashing details, port identification, download-mode buttons, and the
expected-but-scary `Error 1` at the end of an upload are all covered in
[FLASHING.md](FLASHING.md).

## Rollback / recovery

Vendor stock binaries are **not** included in this repo. If you want a way
back to stock, save the vendor images **before** flashing and keep them
somewhere safe; `FLASHING.md` documents where to put them
(`firmware/bin/stock/`) and how to restore them. The ESP32-S3 ROM bootloader
itself is in mask ROM and survives any app flash — worst case you re-flash
over the same ports.

## License

This project's original code is licensed under the MIT License. See
[`LICENSE`](LICENSE).

This code is provided as example/reference material with no warranty and no
support obligation. Use it at your own risk. See
[`docs/legal/DISCLAIMER.md`](docs/legal/DISCLAIMER.md).

Third-party materials keep their original licenses — in particular, the
firmware base under `firmware/` derives from the upstream
`MAKCM_Pass_Package`, which shipped without a license and is treated as
reference material until upstream clarifies. See
[`docs/legal/THIRD_PARTY_NOTICES.md`](docs/legal/THIRD_PARTY_NOTICES.md) and
the plain-English [`docs/legal/LICENSE_GUIDE.md`](docs/legal/LICENSE_GUIDE.md).

## ⚠️ Disclaimer

Flashing firmware can leave your device non-functional if it goes wrong, and
this project is provided **as-is, with no warranty — use at your own risk**.
It was tested on exactly one hardware setup. It is not an official MAKCU
release and is not endorsed by the original author unless upstream says
otherwise. Console behavior (including whether a console tolerates a
passthrough device) is your responsibility to verify. Full version:
[`docs/legal/DISCLAIMER.md`](docs/legal/DISCLAIMER.md).

## Credits

- **MAKCM / MAKCU** by [terrafirma2021](https://github.com/terrafirma2021/MAKCM)
  — the hardware and the original firmware ecosystem.
- The `MAKCM_Pass_Package` example firmware by the same author — the base of
  everything in `firmware/`.
- GIP init sequence derived from the Linux
  [xone](https://github.com/medusalix/xone) driver and TheNathannator's GIP
  protocol notes.

See [docs/legal/THIRD_PARTY_NOTICES.md](docs/legal/THIRD_PARTY_NOTICES.md)
for the full licensing situation.
