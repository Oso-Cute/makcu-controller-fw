# Third-party notices

This repository mixes original work with third-party material. The rule:
**our original code is MIT (see the root `LICENSE`); everything third-party
keeps its own license and its own owner.** We claim no ownership of any
third-party code, binaries, hardware designs, trademarks, or names (MAKCU,
MAKCM, Xbox, kmbox, etc.).

## What is what

| Material | Origin | License status |
|----------|--------|----------------|
| `accessibility/` (Python tools), `firmware/flash_tool.py`, `firmware/Flash_MAKCM.bat`, `docs/ISSUE_REPORT.md`, `FLASHING.md`, READMEs written for this repo | Original to this project | **MIT** (root `LICENSE`) |
| `firmware/MAKCM_ESP32s3_Pass_Left_IDF/`, `firmware/MAKCM_ESP32s3_Pass_Right/` | Upstream `MAKCM_Pass_Package` by the MAKCM author (terrafirma2021), plus this repo's modifications (GIP init fix, accessibility commands) | **No license shipped with the package — treat as reference/example only.** See below. |
| `firmware/bin/MERGED_*.bin` | Compiled from the sources above (plus Espressif ESP-IDF / Arduino-ESP32 components pulled in at build time) | Same status as the sources they were built from |
| `docs/UPSTREAM_PACKAGE_README.md` | The upstream package's original README, preserved unmodified as technical reference | Upstream author's document |

## The unlicensed firmware base

The `MAKCM_Pass_Package` zip this repo's firmware derives from was released
by its author as unfinished example code, **without a license file or license
statement**. A "here's some example code, no support" note on a forum or
Discord is **not** a license — it does not grant redistribution,
modification, or commercial rights.

The author's main MAKCM repository
(<https://github.com/terrafirma2021/MAKCM>) is **GPL-3.0**, but we cannot
confirm the package was published under the same terms. Until the upstream
author clarifies:

- Treat the `firmware/` sources and the binaries built from them as
  **reference/example material, all rights reserved by the original
  author** — shared here in the spirit the package was shared in, not as
  something we own or relicense.
- If upstream confirms a license (GPL-3.0 or otherwise), this repo should
  adopt it for the firmware. Note that **if GPL-3.0 applies, it is
  share-alike**: distributing the firmware or derivatives (including
  compiled binaries) requires making the corresponding source available
  under GPL-3.0 too — the source is already in this repo, so that would be
  satisfied, but downstream users must carry the same obligation forward.
- If you are the upstream author and want this taken down, relicensed, or
  upstreamed, please open an issue.

## Other third-party components

- **Espressif ESP-IDF / Arduino-ESP32 / TinyUSB** — pulled in by PlatformIO
  at build time (not committed here). Predominantly Apache-2.0 / MIT / BSD;
  some vendor-contributed ESP32 components in the wider ecosystem carry the
  **Mulan PSL v2** (a permissive Chinese license). None of these are
  redistributed in this repo as source, but the compiled binaries in
  `firmware/bin/` contain their object code — their permissive terms allow
  this with notice, which this file provides. Keep any license headers you
  encounter intact.
- **GIP protocol knowledge** — the init sequence was derived from the
  GPL-2.0 [xone](https://github.com/medusalix/xone) Linux driver and
  TheNathannator's GIP protocol documentation. Protocol facts (byte layouts)
  were used, not copied code.

## House rules for adding third-party material

Anything third-party added to this repo in the future gets its own
clearly-named folder, keeps the component's original license/notice files
unmodified, and gets an entry in this file (what it is, where it came from,
its license). Code with no clear license does not get committed.
