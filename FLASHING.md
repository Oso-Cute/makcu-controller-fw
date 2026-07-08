# Flashing the MAKCM

**Board:** ESP32-S3 ×2, 4 MB flash, **DIO** @ 80 MHz. Flash baud 921600.
(The images boot in DIO mode — flashing them with QIO headers leaves the MCU
unable to boot; the controller then blinks forever in its announce loop.)

> ⚠️ **You must flash BOTH MCUs.** The Left and Right firmware share a custom
> IPC protocol; a board with only one side flashed (or a stock Right MCU)
> passes **no controller input at all**. This is the single most common
> failure — see [docs/ISSUE_REPORT.md](docs/ISSUE_REPORT.md).

## The three USB ports

| Port | Role | Flashing |
|------|------|----------|
| **USB1 (left)** | Left MCU — USB device to the console/PC | Hold the USB1 boot button while plugging into the PC → appears as `303A:0009` (e.g. COM3) |
| **USB2 (middle)** | CH343 USB-serial bridge — command port (VID `1A86`) | **Do not flash over this port** — auto-reset isn't wired |
| **USB3 (right)** | Right MCU — USB host for the real controller | Just plug into the PC → appears as `303A:1001`, no button needed |

Flash with only one MCU plugged into the PC at a time so you can't target the
wrong port.

## Option A — guided tool (easiest)

```
pip install pyserial esptool
python firmware/flash_tool.py
```

Walks you through detect → flash Left → flash Right → reconnect, using the
prebuilt merged images in `firmware/bin/`. `firmware/Flash_MAKCM.bat` is a
Windows double-click wrapper for the same tool.

## Option B — esptool with the merged images

```bash
# Left (hold the USB1 boot button while plugging in first):
esptool.py --chip esp32s3 --port COM3 --baud 921600 write_flash 0x0 firmware/bin/MERGED_left.bin

# Right (plug USB3 in, no button):
esptool.py --chip esp32s3 --port COM4 --baud 921600 write_flash 0x0 firmware/bin/MERGED_right.bin
```

Replace the COM ports with whatever appears on your machine. The Espressif
Flash Download Tool GUI also works: chip = ESP32-S3, image @ `0x0`,
SPI = DIO / 80 MHz.

## Option C — build from source and flash with PlatformIO

```bash
# Left — quiet build (use this for play):
PLATFORMIO_BUILD_FLAGS="-DCOM3_LOG=0 -DKM_DIAG=0 -DKM_RING=0 -DLAT_DIAG=0" \
  pio run -d firmware/MAKCM_ESP32s3_Pass_Left_IDF -e LEFT_IDF -t upload --upload-port COM3

# Left — log build (verbose diagnostics; floods the command port, not for play):
pio run -d firmware/MAKCM_ESP32s3_Pass_Left_IDF -e LEFT_IDF -t upload --upload-port COM3

# Right:
pio run -d firmware/MAKCM_ESP32s3_Pass_Right -e RIGHT -t upload --upload-port COM4
```

PowerShell equivalent for the build flags:

```powershell
$env:PLATFORMIO_BUILD_FLAGS="-DCOM3_LOG=0 -DKM_DIAG=0 -DKM_RING=0 -DLAT_DIAG=0"
pio run -d firmware/MAKCM_ESP32s3_Pass_Left_IDF -e LEFT_IDF -t upload --upload-port COM3
```

## Entering download (bootloader) mode

- **Left:** hold the boot button next to USB1 while plugging USB1 into the
  PC, then release. A `303A:0009` COM port appears.
- **Right:** none needed — plug USB3 in and it's flashable directly.
- If an upload fails to sync ("Connecting….." then timeout): hold **BOOT**,
  tap **RESET/EN**, release **BOOT**, retry. Lower speed with
  `--upload-speed 115200` if it still fails.

## "Error 1" after a successful write — NORMAL

Flashing over the ESP32-S3 USB-Serial/JTAG port ends like this:

```
Wrote 311808 bytes ... Hash of data verified.
Leaving...
esptool.py can not exit the download mode over USB. To run the app, reset the chip manually.
*** [upload] Error 1
```

**If you see "Hash of data verified", the flash SUCCEEDED.** esptool just
can't reboot the chip over that port. Power-cycle to boot the new firmware.

## After flashing — power-cycle, then verify

1. **Unplug everything, wait a few seconds, replug.** The Right MCU stays
   silent on old firmware until a full power-cycle — this is required, not
   optional.
2. Reconnect for use: PC → USB2 (middle), console → USB1 (left),
   controller → USB3 (right).
3. **If the console doesn't respond to input, replug USB1 last.** The console
   only inspects the device on attach — if it enumerated while the board was
   still coming up after a flash, it marks the device dead and never looks
   again. Unplug USB1, wait ~5 s, replug once everything else is up.
4. On the CH343 command port (4000000 baud 8N1):
   `python accessibility/makcu_access.py` → expect `kmbox: 1.0.0 …`.
5. `python accessibility/makcu_monitor.py` → should stream `KMS …` telemetry
   while you move the sticks.

## If it won't connect

- Wrong COM port — check Device Manager, unplug the other MCU.
- Not in download mode — do the BOOT+RESET sequence above.
- CH343 driver missing — install the WCH CH343 Windows driver.

## Rollback to vendor stock

Stock vendor binaries are **not distributed with this repo**. If you saved
them (do this before your first flash — dump with
`esptool.py read_flash 0 0x400000 stock_left.bin` per side, or keep the
vendor's own download), put them in `firmware/bin/stock/` and flash the same
way as Option B. A stock full-flash image goes to offset `0x0`; an app-only
image goes to `0x10000` (per `partitions/partition_MAKCM.csv`) — if one
offset misboots, try the other. Stock firmware does not include the GIP fix
or the accessibility features.

> ⚠️ Flashing is at your own risk. Verified on one board; the ROM bootloader
> can't be overwritten by these commands, so a bad app flash is recoverable
> by re-flashing, but treat anything you can't re-download as precious and
> back it up first.
