> **Note:** this is the original README that shipped with the upstream
> `MAKCM_Pass_Package`, preserved unmodified as the technical reference for
> the passthrough internals, pin map, `km.*` serial API, and known
> limitations. It describes the package's original purpose in the author's
> own words; this repo's changes and focus are described in the root
> [README](../README.md) and [ISSUE_REPORT.md](ISSUE_REPORT.md).

# MAKCM ESP32-S3 Game Controller Passthrough + KM Injection

Two-MCU firmware that puts a real game controller behind a transparent
ESP32-S3 USB passthrough so a second PC running km-style cheat software
can inject right-stick movement and button presses into the controller's
USB report stream while the user is still actively holding the
controller. The target PC sees exactly the original controller's VID/PID,
descriptors, and protocol — only the right-stick and certain buttons are
mixed in by the firmware.

---

## Features

- **Transparent USB device passthrough.** Left MCU enumerates against the
  target PC using the descriptors snapshotted from the real controller
  by the Right MCU. All control transfers (class, interface, and
  device-recipient vendor) are forwarded live to the real device, so
  Windows sees the real device's exact responses — MS OS 1.0, PowerA-
  style register-access, etc.
- **User-priority XIM-style stick blend.** Aligned signs: inject scales
  by the user's remaining headroom `(1 − |real|/32768)`. Opposing signs:
  inject passes at full gain so the cheat can drag the stick back
  through the user's deflection. User's deflection is always fully
  present in the output.
- **XIM-fit windowed velocity curve.** `km.move(dx,dy)` sums dx/dy into
  an 8 ms drain accumulator; each tick the accumulator drives
  `rx_stick = clamp(C × |accum|^P, ±32767)`. Mouse stops → zero accum
  → stick returns to neutral with zero overshoot. No decay, no carryover,
  no rate-limit.
- **Synthesis timer fills GIP poll gaps.** GIP controllers (Scuf /
  Xbox One / PowerA / Elite / GameSir) only emit a USB report on
  change. The firmware caches the most recent real input report and
  re-submits it (with km_apply overlaid) every 4 ms while injection is
  active, so a brief injection window cannot fall between two real
  reports and be invisible to Windows.
- **Falling-edge release frame.** One last synth frame is emitted on
  the active→inactive transition so the host sees a clean release
  (e.g. RT goes from full to the controller's actual value at the
  moment the km.click pulse ends).
- **Idle housekeep at 10 s of no km activity.** Drops the synth template
  cache on every IN endpoint and zeros the click latch — defends
  against a stuck `tpl_have` replaying old reports and against the
  click_release_ms signed-compare wrap at ~24.85 d uptime.
- **2 s stale-clear for stuck control transfers.** If `FRAME_CTRL_STATUS`
  from Right is lost (CRC drop), `ctl_pending` would otherwise stay
  true forever and stall EP0; the SETUP handler force-clears after 2 s.
- **Hot-replug without PHY churn.** `tud_disconnect` / `tud_connect`
  (esp_tinyusb 1.4.x can't cleanly install→uninstall→install — second
  install returns `ESP_ERR_INVALID_STATE`).
- **kmbox v1.0.0 handshake.** `km.version()` returns the kmbox
  identification line on the KM UART, so unmodified kmbox clients
  detect the device.
- **Two build flavors** from one source tree (LEFT MCU):
  - **Log build** — `COM3_LOG=1`, `KM_DIAG=1`, `KM_RING=1`, `LAT_DIAG=1`.
    Verbose UART0 trace of every km command, every IN/OUT, every
    control transfer, latency stats, and a 64 KB ring drain on cheat
    idle. Use for diagnostics.
  - **Quiet build** — all four = 0 via `-D` flags. Gameplay build.
    `km.version()` still responds (via `km_uart_write_raw`).

---

## Hardware / wiring

Two ESP32-S3 modules. The standard "MAKCM" board layout is assumed; a
`Devkit.json` variant is included for the Right project on 16 MB Devkit
boards.

```
[ target PC ]
      |  USB
      v
+-----------+                +-----------+              +------------------+
| LEFT MCU  |  UART1 (IPC)   | RIGHT MCU |  USB-OTG     |  real controller |
| (device)  |<-------------->| (host)    |<------------>|  (Scuf/DS5/...)  |
+-----------+   5 Mbps       +-----------+
      |                            ^
      | UART0 (KM)                 |
      | 4 Mbaud                    | (Right's `Serial` is silent in host mode)
      v                            |
   [ CH343 ]                       |
      |  USB                       |
      v                            |
[ second PC, COM3 ]                |
   km-sender                       |
   software                        |
```

**Pin map:**

| Signal              | LEFT pin (ESP32-S3) | RIGHT pin (ESP32-S3) |
|---------------------|---------------------|----------------------|
| IPC UART1 TX        | GPIO 2              | GPIO 1               |
| IPC UART1 RX        | GPIO 1              | GPIO 2               |
| KM UART0 TX (→ COM3)| GPIO 43             | —                    |
| KM UART0 RX (← COM3)| GPIO 44             | —                    |
| Diag LED            | GPIO 48             | GPIO 48 (RGB)        |

IPC = 5 Mbps 8N1. KM = 4 Mbaud 8N1.

**MCU roles:**

- **LEFT MCU = USB *device*.** Plugs into the target PC. Acts as the
  controller. Owns:
  - the descriptor cache (built from IPC frames)
  - the TinyUSB stack + custom class driver
  - the km_inject pipeline (8 ms drain, blend, protocol adapters)
  - the KM-command UART (ASCII line parser)
- **RIGHT MCU = USB *host*.** The real controller plugs into its
  USB-OTG. Owns:
  - the ESP-IDF `usb_host` client (enumerate / claim / submit)
  - descriptor snapshotting and IPC shipping
  - URB forwarding (IN/OUT/control) to/from Left

---

## Connection order

1. Power up both MCUs (USB into the second PC's CH343 powers Left;
   Right is powered by the target PC or its own supply).
2. Plug LEFT into the target PC.
3. Plug the real controller into RIGHT.
4. Watch the LEDs:
   - Right's RGB walks IDLE → HOST_INSTALL → CLIENT → NEW_DEV →
     DESC_SENT → DEVICE_READY (steady white).
   - Left's plain LED walks slow → 1.3 Hz → 2 Hz → 4 Hz → 10 Hz
     (host-visible).
5. The target PC enumerates against the real controller's
   VID/PID/strings.
6. Open the km-sender software on the second PC, point it at the
   CH343's COM port (4 Mbaud 8N1).

To swap controllers, just unplug the real controller from Right — Left
drops the D+ pull-up and the target PC sees the device removed. Plug
the new controller in and the cycle re-runs without a Left replug.

---

## Accepted KM API (ASCII, one command per line, `\n` or `\r` terminated)

All commands are delivered on Left's UART0 via the CH343 bridge.

### Handshake

| Command          | Effect                                                               |
|------------------|----------------------------------------------------------------------|
| `km.version()`   | Replies `kmbox:   1.0.0 <date> <time>\r\n>>> ` (kmbox v1.0.0 line).  |

### Movement

| Command                          | Effect                                                  |
|----------------------------------|---------------------------------------------------------|
| `km.move(dx,dy)`                 | Sum dx/dy into the 8 ms drain accumulator.              |
| `km.move_auto(dx,dy,duration)`   | Same as `km.move` — duration argument is ignored.       |
| `km.move_bezier(dx,dy,...)`      | Same as `km.move` — path arguments are ignored.         |

### Click pulse (auto-release after 120 ms)

| Command         | Effect                                                                 |
|-----------------|------------------------------------------------------------------------|
| `km.click(0)`   | Pulse RT / right trigger / "fire" (analog=1023, button bit set).       |
| `km.click(1)`   | Pulse LT / left trigger / "ADS".                                       |
| `km.click(2)`   | Pulse X (face left / Square).                                          |

### Hold / release

| Command              | Effect                            |
|----------------------|-----------------------------------|
| `km.left(1)`         | Hold RT / fire.                   |
| `km.left(0)`         | Release.                          |
| `km.right(1)` / `(0)`| Hold / release LT / ADS.          |
| `km.middle(1)` / `(0)`| Hold / release X (face left).    |
| `km.btnA(1)` / `(0)` | Hold / release A / Cross / South. |
| `km.btnB(1)` / `(0)` | Hold / release B / Circle / East. |
| `km.btnX(1)` / `(0)` | Hold / release X / Square / West. |
| `km.btnY(1)` / `(0)` | Hold / release Y / Triangle / N.  |
| `km.lb(1)` / `(0)`   | Hold / release LB.                |
| `km.rb(1)` / `(0)`   | Hold / release RB.                |

### Unsupported (silently ignored)

- `km.moveto(...)` — would require absolute positional state.
- `km.aim_mode(...)` — single-mode firmware.
- Smooth-move (any `km.move` with a non-zero duration arg) — incompatible
  with the windowed-drain model.

---

## Known controllers

| Family               | Detection                                       | Notes                                                          |
|----------------------|--------------------------------------------------|----------------------------------------------------------------|
| Xbox One / GIP       | IN EP `0x82`, first byte `0x20`, len ≥ 20       | Scuf Instinct, PowerA, Xbox Elite, GameSir, stock Xbox One.    |
| XInput (Xbox 360)    | IN EP `0x81/0x82`, bytes `00 14 ...`, len ≥ 14 | 20-byte XInput report format.                                  |
| DualShock 4 / 5      | First byte `0x01`, len ≥ 64                     | DS5 + DS4 sharing the same 64-byte HID report shape.           |

Other controllers will enumerate transparently (passthrough is descriptor-
agnostic) but `km_apply` will not modify their reports unless their
on-wire format matches one of the three formats above.

---

## Known limitations

- **No absolute positional state.** `km.moveto` is dropped; injection
  is purely delta-based.
- **No duration / bezier path handling.** `km.move_auto` and
  `km.move_bezier` collapse to `km.move(dx,dy)`.
- **One control transfer in flight at a time.** Right serializes
  controls; Left rejects a second SETUP while one is pending (2 s
  stale-clear timer protects against lost STATUS frames).
- **Synth fallback only for known report formats.** Synth templates
  are only cached for GIP `0x20`, XInput `00 14`, and DS5 `0x01`
  reports. Heartbeats / announces are not cached (they carry no stick
  bytes).
- **Alt-setting changes don't reopen endpoints on Right.**
  `SET_INTERFACE(iface, alt > 0)` is forwarded as a control transfer
  but endpoints declared on the new alt are not opened. Stick / button
  injection still works on whatever was opened at SET_CONFIG time.
- **String descriptors via `usb_device_info`.** Right uses the
  ESP-IDF-resolved manufacturer/product/serial strings rather than
  explicit `GET_DESCRIPTOR(STRING, idx, 0x0409)` reads, so non-standard
  strings (e.g. interface strings, the 0xEE MS OS 1.0 string) may not
  be byte-exact replays.
- **TinyUSB stack stays installed for life.** Hot-swap uses
  `tud_disconnect` / `tud_connect` — esp_tinyusb 1.4.x can't
  install→uninstall→install (second install returns
  `ESP_ERR_INVALID_STATE`).
- **8 endpoints, 4 interfaces, 8 strings max** on Left.
- **64 B max-packet size on every endpoint** (full-speed only; no HS).
- **Verbose log build floods COM3.** Use the quiet build for gameplay;
  the log build is for diagnostics only.

---

## Build

Both projects use PlatformIO. The Left project is pure ESP-IDF
(framework=espidf); the Right project is Arduino-on-ESP32 with the
ESP-IDF `usb_host` client included via the bundled core.

```
# Left, default = log build:
pio run -d MAKCM_ESP32s3_Pass_Left_IDF -e LEFT_IDF

# Left, quiet build:
PLATFORMIO_BUILD_FLAGS="-DCOM3_LOG=0 -DKM_DIAG=0 -DKM_RING=0 -DLAT_DIAG=0" \
  pio run -d MAKCM_ESP32s3_Pass_Left_IDF -e LEFT_IDF

# Right:
pio run -d MAKCM_ESP32s3_Pass_Right -e RIGHT
```

---

## Future enhancements

- **Absolute `km.moveto` (with positional state).**
- **True `km.move_auto` / `km.move_bezier`** that respect duration and
  bezier control points across multiple drain windows.
- **Multi-mode `km.aim_mode`** (e.g. swap between curves at runtime).
- **Binary KM transport** (current parser is ASCII-only; the binary
  fast-path in `km_ingest_raw` is unused).
- **Byte-exact string descriptor replay** via explicit
  `GET_DESCRIPTOR(STRING, idx, 0x0409)` on Right.
- **`SET_INTERFACE(iface, alt > 0)` endpoint re-open** on Right.
- **OUT-transfer backpressure / coalescing** on Right.
- **Per-controller binary commands** beyond the generic km API
  (e.g. rumble injection, lightbar set).

---

## File layout

```
MAKCM_Pass_Package/
├── README.md                            this file
├── MAKCM_ESP32s3_Pass_Left_IDF/         LEFT MCU project (ESP-IDF)
│   ├── platformio.ini
│   ├── sdkconfig.defaults
│   ├── CMakeLists.txt
│   ├── boards/MAKCM.json
│   ├── partitions/partition_MAKCM.csv
│   ├── include/pass_ipc.h
│   └── src/
│       ├── CMakeLists.txt
│       ├── idf_component.yml
│       ├── main.c                       USB lifecycle + descriptor cache
│       ├── ipc.c                        UART1 IPC + UART0 KM channel
│       ├── km_inject.c                  injection pipeline + parser
│       └── pass_usb_device.c            TinyUSB custom class driver
└── MAKCM_ESP32s3_Pass_Right/            RIGHT MCU project (Arduino + IDF)
    ├── platformio.ini
    ├── boards/MAKCM.json
    ├── boards/Devkit.json               16 MB Devkit alternate
    ├── partitions/partition_MAKCM.csv
    ├── include/
    │   ├── pass_ipc.h
    │   ├── PassUsbHost.h
    │   └── diag.h
    └── src/
        ├── main.cpp                     entry + IPC dispatch
        ├── ipc.cpp                      UART framer
        ├── diag.cpp                     RGB LED state machine
        └── PassUsbHost.cpp              ESP-IDF usb_host wrapper
```
