# third_party/

Home for third-party code, binaries, or examples that get added to this repo
in the future. Currently empty on purpose — the firmware base lives in
`firmware/` with its status documented in
[`docs/legal/THIRD_PARTY_NOTICES.md`](../docs/legal/THIRD_PARTY_NOTICES.md).

Rules for putting anything here:

1. **Only if its license allows redistribution.** Check before committing.
2. **One subfolder per component**, keeping the component's original
   `LICENSE` / `COPYING` / `NOTICE` files and any license headers inside it,
   unmodified.
3. **Add an entry to `docs/legal/THIRD_PARTY_NOTICES.md`** saying what it
   is, where it came from, and its license.
4. **No random copied code without a clear license.** Unlicensed material is
   all-rights-reserved by default and must not be committed — the only
   exception is small, clearly-marked reference notes (facts, byte layouts,
   pinouts), never copied source.
