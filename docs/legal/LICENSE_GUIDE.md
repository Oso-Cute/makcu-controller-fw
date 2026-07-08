# License quick guide (plain English, not legal advice)

A cheat sheet for the licenses that matter around this project.

## MIT
- You **can**: use, copy, modify, merge, sell, and close-source it.
- You **must**: keep the copyright + license notice in copies and
  substantial portions.
- No warranty; author not liable.
- This repo's original code is MIT.

## Mulan PSL v2
- Permissive, roughly MIT-like, dual-language (Chinese/English) license used
  by some Chinese-origin embedded/vendor code.
- You **can**: use, modify, distribute, sell, sublicense.
- You **must**: keep the license text and copyright notices.
- Explicitly grants patents, explicitly disclaims trademark rights.
- If a vendor file's header says Mulan PSL v2, keep that header intact.

## GPL (v2 / v3)
- **Not just credit — copyleft.** Using GPL code in a work you distribute
  can require the whole combined/derivative work's source to be available
  under the GPL too.
- You **can**: use, modify, distribute, even sell.
- You **must** (when distributing): provide the corresponding source, keep
  the GPL license and notices, license your modifications under the GPL.
- Private/internal use has no sharing obligation — the duties trigger on
  **distribution**.
- Relevant here: upstream MAKCM's main repo is GPL-3.0, and the xone driver
  (GIP protocol reference) is GPL-2.0.

## No license at all
- Default copyright law applies: **all rights reserved by the author.**
- You may look at it and learn from it; you may **not** legally copy,
  redistribute, modify-and-ship, or use it commercially without permission.
- Treat it as reference/example only until the author grants a license.
- Relevant here: the `MAKCM_Pass_Package` firmware base shipped with no
  license — see `THIRD_PARTY_NOTICES.md`.

## "Example code, no support" posted on Discord/forums
- That is a **support statement, not a license.** It sets expectations
  ("don't ask me for help") but grants no copyright permissions.
- Without an actual license grant, such code is in the "no license" bucket
  above.

When in doubt: keep every notice you find, don't relicense other people's
work, and ask the author.
