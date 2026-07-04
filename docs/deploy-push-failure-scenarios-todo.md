# TODO: failure scenarios when pushing app code to the device (`tools/deploy_app.py`)

Status: **living checklist** — started 2026-07-03 (builder repo). Companion to
`docs/mpy-precompile-deploy-plan.md` (the .mpy-precompile change) and the incident write-up in
`docs/knowledge/deploy-serial-truncation.md`.

Purpose: enumerate the ways a push of Python/`.mpy` code onto the ESP32-P4 over the held-open
raw-REPL serial link can go wrong, what each looks like, what already guards against it, and what
still needs hardening. The through-line: **a push can leave the device running code that is not
what you think you deployed.** Treat every deploy as untrusted until `verify: PASS` + an
`import-smoke` `SMOKE_OK`.

---

## 1. Silent serial truncation (dropped USB-CDC chunk)  — *highest risk*
- **Symptom:** either a hard abort mid-push — `ValueError: incorrect padding` in device `_w`
  (a base64 batch was cut) — or, worse, a *silent* short write: the file lands truncated, still
  imports (valid up to the cut), and fails later as a deterministic `ImportError: can't import
  name X` that does **not** reproduce on host. Full incident: `deploy-serial-truncation.md`.
- **Cause:** the P4 USB-CDC "resets racily on open" and drops bytes on multi-KB transfers. The
  bigger the file / batch and the more files pushed at once (e.g. `--clean`), the more exposed.
- **Now mitigated:** `deploy_app.py` pushes with a verify+auto-retry loop (`PUSH_ATTEMPTS`): each
  round re-reads the device tree so only missing/size-mismatched files re-push, a batch that
  raises is retried, and the deploy **refuses to write `/main.py` / boot** unless `verify()` is
  clean across the whole `expected` set.
- **Still TODO:**
  - Verify each file's size (or a cheap hash) **immediately after writing it** and retry that one
    file, instead of relying on a whole-round re-push driven by the end-of-round size compare.
  - Consider smaller chunks / flow control on the P4 CDC for the large-file (`_push_big`) path.
  - Fold `ensure_dev_deps` (secp256k1 shim, any stdlib vendor) into the verified `expected` set —
    those writes are currently outside the retry loop.

## 2. `.mpy` ABI / version mismatch
- **Symptom:** device raises `ValueError: incompatible .mpy file` at import of any pushed module.
- **Cause:** the `.mpy` was produced by an mpy-cross whose `MPY_VERSION`/`MPY_SUB_VERSION` differs
  from the firmware's (e.g. a `pip install mpy-cross`, or a stale build from a different
  MicroPython). ABI is pinned per MicroPython version.
- **Now mitigated:** `deploy_app.py` uses the mpy-cross built from **this tree's pinned submodule**
  (`deps/micropython/upstream/mpy-cross`) and logs its `--version` banner. Same source as the
  firmware ⇒ same ABI (verified: emits `mpy v6.3`, matches `py/persistentcode.h` MPY_VERSION 6 /
  SUB 3).
- **Still TODO:** assert the emitted mpy version against the firmware's `MPY_VERSION` before the
  first push and fail fast with a clear message, rather than discovering it as an on-device import
  error.

## 3. Native-arch `.mpy` rejection
- **Symptom:** `mpy-cross` errors at compile (`SyntaxError: invalid arch`), or a native `.mpy`
  built with the wrong `-march` is rejected on-device.
- **Cause:** `@micropython.native` / `@viper` / inline-asm modules need a target `-march`
  (S3 = `xtensawin`, P4 = `rv32imc`), and native `.mpy` must also match `MPY_SUB_VERSION` **and**
  arch — far more fragile than pure-Python bytecode.
- **Now mitigated:** **compile-with-fallback** — modules mpy-cross can't compile are shipped as raw
  `.py` (they compile on-device only if imported). Today that's exactly one file,
  `hardware/displays/st7789_mpy.py`, which the ESP32 build never uses (display is the LVGL C
  module). Each fallback prints a `ship source (...)` line.
- **Still TODO:** if a native module is ever actually needed on ESP32, prefer **freezing it into
  firmware** over per-board `-march` `.mpy` on the VFS.

## 4. Stale counterpart shadowing (`.py` ↔ `.mpy`)
- **Symptom:** device keeps running old code after a deploy; edits appear to have no effect.
- **Cause:** the importer prefers `.mpy` over `.py`. A leftover `.mpy` from a prior `--mpy` deploy
  shadows a freshly pushed `.py` (`--source` mode), or vice-versa.
- **Now mitigated:** for every module pushed, `push_tree` removes the superseded counterpart on the
  device (drops the old `.py` when shipping `.mpy`, and the old `.mpy` when shipping `.py`).
- **Still TODO:** prune **orphan** device files that are no longer in `expected` at all (a module
  deleted from source stays on the device unless `--clean`). Add an opt-in "delete extras" pass.

## 5. Partial/aborted deploy leaves an unbootable tree
- **Symptom:** device won't boot or throws import errors after a deploy that crashed mid-push.
- **Cause:** an abort (see #1) after `--clean` has wiped `/lib/seedsigner` + `/lib/embit` leaves a
  half-written tree. `--clean` widens this window (wipe-then-repush); a plain incremental push is
  safer under mid-failure because the previous good files remain.
- **Now mitigated:** the retry loop completes the push before returning, and boot is gated on
  `verify: PASS`. On persistent failure the tool **exits non-zero without booting**.
- **Still TODO:** consider staging into a temp dir on-device and swapping, so a failed push never
  degrades the currently-booting tree.

## 6. Pushing a drifting working tree instead of a committed tip
- **Symptom:** the device runs half-finished/experimental code; a deploy isn't reproducible.
- **Cause:** the source clone is being edited concurrently, so the live working tree ≠ the intended
  commit at push time.
- **Now mitigated:** `--seedsigner-src` / `--embit-src` overrides let the push be pinned to a
  `git archive` export of a committed tip (e.g. `integration/lvgl-mpy`) instead of the live tree.
- **Still TODO:** bake `--ref <gitref>` support directly into `deploy_app.py` (archive the ref to a
  temp dir internally) so the pin is one flag, and record the deployed commit hash on-device
  (e.g. `/lib/.deployed_ref`) for after-the-fact provenance.

## 7. Incremental size-match false-negative
- **Symptom:** a changed module isn't updated on the device.
- **Cause:** the incremental skip compares **size only**; a change that compiles to the same byte
  count is skipped.
- **Now mitigated:** `--clean` or `--force` re-pushes unconditionally; probability is low.
- **Still TODO:** hash-based compare instead of size for the skip decision.

## 8. Import-closure / MicroPython-compat failures (not a transport failure)
- **Symptom:** a module imports on host/CI but fails on device.
- **Cause:** MicroPython stdlib gaps, frozen-module assumptions, or a stale `secp256k1` dev shim.
- **Now mitigated:** `--mode import-smoke` imports `seedsigner.controller` on-device and reports the
  full traceback; the module-pop step forces a clean re-import of app/embit/secp256k1.
- **Still TODO:** none here specifically — this belongs to the mpy-compat workstream, listed so a
  push failure isn't misattributed to the transport.

## 9. REPL busy / device mid-reset on open
- **Symptom:** deploy hangs or errors entering the raw REPL.
- **Cause:** the app is in a tight loop, or the CDC is mid-reset when we poll-open without a hard
  reset.
- **Now mitigated:** poll-open with retry; a hard-reset path exists in `sd_format_push`.
- **Still TODO:** optional `--reset` before push for a known-quiescent starting state.

---

## Hardening backlog (condensed, priority order)
1. Per-file post-write size/hash check + targeted retry (covers #1 at the file granularity).
2. Assert mpy version vs firmware before push (#2).
3. Prune device orphans not in `expected` (#4/#5).
4. `--ref` git-pin + record deployed commit on-device (#6).
5. Hash-based incremental skip (#7).
6. Fold dev-deps into the verified/retried set (#1 tail).
