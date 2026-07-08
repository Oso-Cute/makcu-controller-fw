# Firmware

Two PlatformIO projects — one per MCU. **Both must be flashed** (they share a
custom IPC protocol); see the repo root [FLASHING.md](../FLASHING.md).

| Path | MCU | Framework | What it does |
|------|-----|-----------|--------------|
| `MAKCM_ESP32s3_Pass_Left_IDF/` | Left | ESP-IDF (TinyUSB) | Presents as the controller to the console/PC. Owns the descriptor cache, the report-rewrite pipeline (`src/km_inject.c` — steady filter, trim, telemetry, button latches live here), and the serial command channel. |
| `MAKCM_ESP32s3_Pass_Right/` | Right | Arduino + ESP-IDF `usb_host` | Enumerates the real controller, relays descriptors and USB transfers to the Left over a 5 Mbps UART. The **GIP init handshake fix** lives in `src/PassUsbHost.cpp`. |
| `bin/` | — | — | Prebuilt merged flash images built from this source (see `bin/README.md`). |
| `flash_tool.py` | — | — | Guided GUI flasher (`pip install pyserial esptool`, then `python flash_tool.py`). `Flash_MAKCM.bat` is a Windows double-click wrapper. |

## Changes vs the upstream package

The base is the author's `MAKCM_Pass_Package` (its original README is
preserved at [../docs/UPSTREAM_PACKAGE_README.md](../docs/UPSTREAM_PACKAGE_README.md)
— still the best technical reference for the passthrough internals, pin map,
and the `km.*` serial API). This repo adds:

- **Right MCU: GIP init handshake** (identify / power-on / LED) so Xbox One
  GIP controllers actually start streaming input. Without this the stock
  package delivers no input at all. Details in
  [../docs/ISSUE_REPORT.md](../docs/ISSUE_REPORT.md).
- **Left MCU: accessibility commands** on the serial channel — `km.steady*`
  (tremor low-pass + deadzone), `km.trim` (drift cancel), `km.telem`
  (telemetry stream), plus the existing button hold/latch commands.
- Diagnostic instrumentation used during debugging (harmless in normal use).

## Build

```bash
# Left — quiet build (for play):
PLATFORMIO_BUILD_FLAGS="-DCOM3_LOG=0 -DKM_DIAG=0 -DKM_RING=0 -DLAT_DIAG=0" \
  pio run -d MAKCM_ESP32s3_Pass_Left_IDF -e LEFT_IDF

# Left — log build (verbose diagnostics):
pio run -d MAKCM_ESP32s3_Pass_Left_IDF -e LEFT_IDF

# Right:
pio run -d MAKCM_ESP32s3_Pass_Right -e RIGHT
```

First Left build downloads the ESP-IDF toolchain — expect it to take a while.
