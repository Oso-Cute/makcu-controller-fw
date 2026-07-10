# Prebuilt flash images

Merged full-flash images built from **this branch's** source, flashed at
offset `0x0` (bootloader + partition table + app in one file). The images
always match the checked-out branch — different branches ship different
bins.

**This build: 2026-07-08, `main` branch** (stable release build).

| File | MCU | Build |
|------|-----|-------|
| `MERGED_left.bin` | Left (USB1, console-facing) | Quiet build (`COM3_LOG=0`) + accessibility features |
| `MERGED_right.bin` | Right (USB3, controller host) | Includes the GIP init fix |

Check what is actually flashed on a board: `python accessibility/makcu_access.py`
— the `km.version()` reply contains the Left build date (`Jul  8 2026` = this
build; `Jul  7 2026` = the old pre-GIP-fix build, reflash it).

Target: ESP32-S3, 4 MB flash, DIO @ 80 MHz (do not re-merge with QIO — it
won't boot).

`../flash_tool.py` looks for these files here by default. Flash manually
with:

```bash
esptool.py --chip esp32s3 --port <COM> --baud 921600 write_flash 0x0 <file>
```

If you build from source yourself, the PlatformIO output lands in each
project's `.pio/build/<env>/` — either flash those images directly with
PlatformIO's `-t upload`, or drop your own merged images here (same names)
for the flash tool to pick up.

**`stock/` (not in the repo):** put saved vendor stock binaries here if you
want a rollback path — see the Rollback section of
[../../FLASHING.md](../../FLASHING.md). Vendor binaries are not distributed
with this repo.
