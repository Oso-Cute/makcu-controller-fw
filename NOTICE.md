# Notice — origin, credits, and licensing status

## What this repo is based on

This project is a community cleanup and fix of the **`MAKCM_Pass_Package`**
example firmware for the MAKCM / MAKCU dual-ESP32-S3 board, released by the
MAKCM author as unfinished example code (shared as-is, without support). The
firmware sources under `firmware/` are that package plus this repo's changes
(GIP init handshake on the Right MCU; steady filter, trim, telemetry, and
button latches on the Left MCU). The Python code under `accessibility/` and
the docs are original to this repo.

## Credits

- **terrafirma2021** — author of the MAKCM / MAKCU hardware ecosystem and the
  original firmware, including the base `MAKCM_Pass_Package`.
  Main repo: <https://github.com/terrafirma2021/MAKCM> (GPL-3.0).
- **medusalix's [xone](https://github.com/medusalix/xone) driver** and
  **TheNathannator's GIP protocol documentation** — sources for the GIP init
  byte sequences (identify / power-on / LED).

## Licensing status — read before reusing

The `MAKCM_Pass_Package` zip this repo derives from **did not include a
license file or license statement**. The author's main MAKCM repository is
GPL-3.0, but we cannot confirm the package was published under the same terms,
so **this repo deliberately does not attach its own LICENSE file** — inventing
one for someone else's code would be wrong.

Practical consequences:

- Treat the firmware sources as **all rights reserved by the original author
  until clarified**. This repo exists as working notes / a community fix, in
  the spirit the package was shared in.
- If the upstream author confirms a license (or that the package falls under
  the main repo's GPL-3.0), this repo should adopt it — contributions toward
  getting that clarified are welcome.
- The original files under `accessibility/`, `docs/ISSUE_REPORT.md`, and the
  build/flash tooling written for this repo may be treated as available under
  the same license the firmware ends up under, so nothing here becomes more
  restrictive than upstream.

If you are the upstream author and want this taken down, relicensed, or
upstreamed, please open an issue.
