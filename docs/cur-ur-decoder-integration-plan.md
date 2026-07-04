# Plan: Switch the ESP32‑P4 (MicroPython) UR decoder to native cUR

> **Start the implementation session in `seedsigner-micropython-builder`** (the critical
> path: ESP‑IDF/CMake/mbedtls wiring, firmware build + flash, on‑device parity/timing).
> Treat `seedsigner` (small app‑side shim, depends on the firmware existing) and the
> `kdmukAI-bot/cUR` fork as sibling dirs to `cd` into. Alternatively launch from the parent
> `/home/kdmukai/dev/` for an equidistant cross‑repo view. Order: fork cUR → builder
> firmware works on‑device → seedsigner shim/adapter → end‑to‑end scan.
>
> **Status (2026‑07‑04):** planning complete; this doc now lives here and is linked from
> `.claude/handoff.md`. **Implementation has not started** — the prior session only produced
> this plan. Begin execution from the *Order* above (fork cUR first).

## Context
Live scanning of animated **BC‑UR** (fountain‑coded) PSBTs on the ESP32‑P4 runs its
per‑frame ingestion entirely in **pure‑Python MicroPython** (the vendored
`src/seedsigner/helpers/ur2/`): a 64‑bit Xoshiro256 PRNG that becomes RV32 bignums,
per‑byte Python XOR reduction, and pure‑Python bytewords + CRC32. On large multi‑part
PSBTs that per‑frame cost plausibly exceeds the camera's inter‑frame interval, so frames
drop and the animation must loop repeatedly → slow, flaky scans. (BBQr per‑frame is
already trivial — it just stores raw strings — so this is UR‑specific.)

**Fix:** bake Odudex's **cUR** (pure‑C BC‑UR, BSD‑2‑Clause‑Plus‑Patent, production‑used in
Krux, same maintainer as **k_quirc** which the P4 camera pipeline already uses) into the
firmware and have `decode_qr` use its native `uUR.URDecoder` on MicroPython, keeping the
pure‑Python `ur2/` decoder as the CPython/host fallback. This mirrors embit's
`secp256k1` native‑or‑fallback pattern and SeedSigner's `compat/*` shim convention.

**Explicitly OUT of scope:** the ~9 s **post‑scan** delay is elliptic‑curve (secp256k1)
work and needs a *separate* native‑module effort — **cUR does not touch it.** cUR speeds
up *live animated‑QR ingestion*, not the parse/sign afterward. Do not conflate the two.

## Scope
- **IN:** P4/MicroPython firmware bakes in cUR; the seedsigner app selects the native
  decoder behind a compat seam; `ur2/` stays as the fallback. **Decode path only.**
- **OUT / follow‑ons:** the UR *encoder* (cUR's `uUR.c` already ships `UREncoder` — a cheap
  later add for animated‑QR *display*, symmetrically pure‑Python‑slow today); Pi Zero
  native + full `ur2/` removal (see **Follow‑ups (TODO)**); the secp256k1 post‑scan
  bottleneck.

## Why cUR fits (verified)
- Decoder API is a near‑exact match to the seam `decode_qr` depends on. Native `uUR`
  exposes `URDecoder` with `receive_part(str)->bool`, `is_complete()->bool`, a `result`
  property (→ `UR` with `.type` + `.cbor` raw CBOR bytes), `expected_part_count`,
  `processed_parts_count`, `estimated_percent_complete()`.
- Same license as the `ur2/` code it shadows; returns **raw CBOR**, so the existing
  frozen `urtypes` still does the tiny final unwrap — hand‑off unchanged.
- Ships its own ESP‑IDF `idf_component_register` `CMakeLists.txt` (+ `UR_USE_MBEDTLS_SHA256`)
  and `uUR.c` MicroPython binding, so almost no glue to author.

## Fork & upstream flow
We build against **our own fork**, never upstream directly — mirroring k_quirc, which is
already consumed as `kdmukAI-bot/k_quirc` on a `seedsigner-dev` branch.
- Fork `odudex/cUR` → **`kdmukAI-bot/cUR`**; do all work on a **`seedsigner-dev`** branch
  off `main` (matches the k_quirc convention and the builder-feature-branch rule).
- The builder submodule points at the fork + branch, pinned to a specific reviewed commit.
- Any local changes (ESP-IDF-version/mbedtls compat, bug fixes, optional binding
  conveniences) land on the fork branch and are **PR'd to Odudex upstream** when ready. cUR
  is a **support repo** (upstream owned by odudex, not the SeedSigner org / embit
  ecosystem), so relaxed PR hygiene applies and the standard co-author trailers are used.
- **Planned upstream contribution — weighted progress estimate (committed):** port
  SeedSigner's weighted‑mixed‑frames method into cUR's `src/fountain_decoder.c` — iterate the
  mixed‑parts set giving 1/|mixed| partial credit per index (per‑index cap 0.75), add the
  recovered `received_part_indexes` count, divide by `expected_part_count` — and expose it via
  a backward‑compatible opt‑in flag on `uUR.c`'s `estimated_percent_complete`. Land on
  `seedsigner-dev`, build the P4 against it, then **open a PR to `odudex/cUR`** (opt‑in flag ⇒
  Krux/others unaffected). Keeps the on‑device progress bar identical to today's.

## Repo A — `seedsigner-micropython-builder` (bake cUR into firmware)
The builder integrates C via **CMake `USER_C_MODULES`**, not the `.mk`/USERMOD path, so
cUR's `micropython.mk` is ignored. Closest precedent to clone: **k_quirc** (git submodule →
plain IDF component → linked into the usermod; the binding `.c` added to usermod
`target_sources` so its QSTRs are scanned).

1. **Submodule:** add **`kdmukAI-bot/cUR` @ `seedsigner-dev`** (pinned commit) as a git
   submodule, placed as an IDF component dir mirroring how `esp-camera-pipeline`/`k_quirc`
   are nested. cUR's root `CMakeLists.txt` registers component `<dirname>` → lib
   `__idf_<dirname>`.
2. **Discover the component:** append cUR's dir to `MICROPY_EXTRA_COMPONENT_DIRS` in
   `scripts/build_firmware.sh` (one `\;`‑appended line, exactly like the
   `esp-camera-pipeline` entries at lines ~60‑70).
3. **Wire the binding:** in `bindings/micropython.cmake` —
   - add cUR's `uUR.c` to `target_sources(usermod_dm INTERFACE ...)` (compiles against
     MicroPython headers; its `MP_REGISTER_MODULE(MP_QSTR_uUR, ...)` + `MP_QSTR_*` get
     QSTR‑scanned);
   - add cUR `src/` + repo root to `target_include_directories(...)` (headers are simple C
     — bind `uUR.c` directly, no thin‑facade split needed);
   - `target_link_libraries(usermod_dm INTERFACE __idf_<cur>)`.
4. **SHA backend:** cUR's `CMakeLists.txt` sets `UR_USE_MBEDTLS_SHA256` and
   `REQUIRES mbedtls mbedtls_compat`. Verify those component names resolve under the
   builder's ESP‑IDF version (cUR/Kern target 6.0.x; builder is on 5.5.x). **If
   `mbedtls_compat` is absent, either adjust `REQUIRES` or fall back to cUR's bundled
   `src/sha256/sha256.c`** (omit the mbedtls define — loses HW‑SHA accel, minor, SHA is a
   small fraction of per‑frame cost).
5. Result: firmware exposes module **`uUR`** (`URDecoder`, `UREncoder`, `UR`, `Types`);
   `urtypes` stays frozen (unchanged).

**Files touched:** `.gitmodules` (+ submodule; possibly a nested `.gitmodules` like
esp-camera-pipeline), `scripts/build_firmware.sh`, `bindings/micropython.cmake`.
`usercmodule.cmake` needs no change (already `include`s `bindings/micropython.cmake`).
**Clone from:** `ports/esp32/camera_scanner/CMakeLists.txt`, `bindings/modcamera_scanner.c`,
and the k_quirc `.gitmodules` entry.

## Repo B — `seedsigner` (select native decoder; keep fallback)
cUR's `uUR.URDecoder` matches the required seam with **two small mismatches** bridged by a
thin Python adapter (trivial, matches the `compat/` idiom):
- `decode_qr` calls `result_message().cbor`; cUR exposes `.result` (property). Adapter adds
  `result_message()` → returns `self._d.result` (a `UR` with `.cbor`).
- `decode_qr` calls `estimated_percent_complete(weight_mixed_frames=…)`; cUR takes no kwarg
  and implements only the **reference** estimate — verified in `src/fountain_decoder.c`:
  `min(0.99, processed_parts_count / (expected_part_count * 1.75))`, i.e. the equivalent of
  SeedSigner's `weight_mixed_frames=False` branch (**pins at 99%** until the final frame).
  cUR has **no** equivalent of SeedSigner's custom `weight_mixed_frames=True` weighted‑mixed
  scoring (that was a SeedSigner‑local enhancement, not in foundation‑ur‑py). **Behavior
  delta:** the scan UI currently passes `weight_mixed_frames=True` (`scan_screens.py:174`)
  for the smoother, recovered‑fragment‑aware bar; an adapter that just swallows the kwarg
  regresses the progress bar to the jumpier, pins‑at‑99% heuristic (functionally fine — it
  still jumps to 100% on completion — but a UX downgrade). **Decision — do the clean fix, not
  the regression:** port SeedSigner's weighted method into cUR's `fountain_decoder.c` on our
  fork (feasible — cUR already tracks `received_part_indexes`, `mixed_parts_hash`,
  `expected_part_indexes`), expose it via a backward‑compatible opt‑in flag on `uUR.c`'s
  `estimated_percent_complete`, and **PR it to Odudex upstream** (see *Fork & upstream
  flow*). The adapter passes `weight_mixed_frames` straight through so the on‑device progress
  bar matches today's behavior (with a `TypeError` fallback for an un‑enhanced build). A pure‑Python reconstruction in the adapter is **not** viable — `uUR.c` exposes
  only `expected_part_count`/`processed_parts_count`, not the mixed‑part inventory.
- `ur2`'s `receive_part` swallows all exceptions; adapter wraps `receive_part` in
  try/except → `False` on bad input so a malformed frame never raises.

**Change:** introduce a compat selector (e.g. a `helpers/ur2` selector module or a
`compat/`‑style shim) doing `try: import uUR` (wrap `uUR.URDecoder` in the adapter)
`except ImportError: from .ur_decoder import URDecoder`. Repoint `decode_qr.py:15`
(`from seedsigner.helpers.ur2.ur_decoder import URDecoder`) at the selector. **Leave
`helpers/ur2/` intact** as the fallback so CPython (Pi Zero) + host pytest are unchanged.
Use the `__import__("uUR")` guard idiom (like `compat/zlib.py`) so the mpy line‑checker
isn't tripped.

Keep the **Python adapter as the primary** home for the two API‑gap bridges — it's
portable and works against unmodified upstream cUR. Since we own the fork, the alternative
is to add a `result_message()` alias + the `weight_mixed_frames` kwarg + non‑raising
`receive_part` directly in `uUR.c` on `seedsigner-dev` and upstream them as a
drop‑in‑compatibility improvement; do that only if the Python adapter proves awkward, so
the seedsigner app isn't coupled to a fork‑only binding change.

**Interface contract the adapter must satisfy** (bound entirely by `decode_qr.py`):
`URDecoder()` · `receive_part(str)->bool` (non‑raising) · `is_complete()->bool` ·
`result_message()->obj with .cbor (raw CBOR bytes)` ·
`estimated_percent_complete(weight_mixed_frames=False)->float in [0,1]`. Must yield valid
`.cbor` for all four UR types routed today: `crypto-psbt`, `crypto-output`,
`crypto-account`, `bytes`.

## Performance expectation (best‑guess, un‑benchmarked)
Moving the per‑frame hot loop from interpreted MicroPython to compiled C:
- **Xoshiro256 `next()`** — pure‑Python does masked 64‑bit ops as heap‑allocated RV32
  bignums; C does native `uint64`. Likely **~100×** per call, and it runs ~`seq_len` times
  per mixed frame.
- **bytewords decode + CRC32** — pure‑Python per‑char/byte loops → C tight loops, **~20‑50×**.
- **XOR reduction** — pure‑Python per‑byte loop → C word‑at‑a‑time, **~50‑100×**.
- SHA256 seeding is already native (`uhashlib`) today; mbedtls HW is a minor extra.

**Aggregate:** per mixed frame from *tens of ms* (interpreted) to *sub‑ms–low‑single‑ms*
(C) — a rough **~30‑100×** per‑frame speedup. The user‑visible win is bigger than the raw
multiple: today the P4 likely can't finish a frame within the camera interval on large
PSBTs (→ dropped frames → many animation loops → slow/flaky). Native decode drops per‑frame
time well under the frame interval, so it keeps up with the camera and completes in ≈ the
minimum frames (~1.75× `seq_len`). Expect large‑PSBT UR scans to go from *multiple passes /
seconds of scanning / occasional failures* to *one–two passes / near‑instant capture*.
**No change** to: the ~9 s post‑scan EC delay (secp256k1, separate), BBQr scanning (already
fast), or the Pi Zero (already fast on CPython).

## Verification
- **Host regression:** existing animated‑QR pytest fixtures still pass on the `ur2`
  fallback (native module absent) — no CPython regressions.
- **On‑device parity:** build firmware; over serial feed a known multi‑part
  `ur:crypto-psbt/...` sequence to both `uUR.URDecoder` and the pure‑Python decoder; assert
  `result_message().cbor` is **byte‑identical**, and `urtypes.crypto.PSBT.from_cbor(cbor).data`
  → the expected PSBT bytes. Repeat for `crypto-output` / `crypto-account` / `bytes`.
- **On‑device A/B benchmark (native vs. pure‑Python) — the headline test.** Drive both
  decoders on‑device with the **btc‑datagen 100‑input UR PSBT** fixture
  (`btc-datagen/output/psbt_2of3_p2wsh_100in_normal_parts.txt` — a 2‑of‑3 P2WSH multisig, the
  animated `ur:crypto-psbt/...` parts; verify the reassembled bytes against
  `..._100in_normal_psbt.txt`). Feed the identical frame list to `uUR.URDecoder` and the
  pure‑Python `URDecoder`, instrumenting `receive_part` with `time.ticks_ms`/`ticks_diff`;
  record, per frame, each decoder's ms **and their differential** (native vs. pure‑Python at
  the same frame index) — this per‑frame delta is the headline number — plus each decoder's
  total wall‑clock to completion; confirm both reach byte‑identical `result_message().cbor`.
  Emit a per‑frame table (frame #, pure/mixed, pure‑Python ms, native ms, Δ) so the
  differential is legible at a glance. Re‑run with the
  `low`/denser‑parts variant to vary `seq_len`. This 100‑input case is a deliberate stress
  test — well past the original 10‑input report — so the native/pure‑Python gap is stark.
- **Progress parity:** with the weighted method ported into the forked cUR, assert
  `estimated_percent_complete(weight_mixed_frames=True)` tracks the pure‑Python decoder's
  values (within rounding) across the frame sequence, so the on‑device progress bar is
  unchanged.
- **End‑to‑end:** scan a 10‑input PSBT on the P4 — confirm no dropped‑frame stalls, faster
  completion, identical parsed overview. (Post‑scan EC delay unchanged, as expected.)
- **mpy checker:** run `tools/mpy_compat_check.py`; confirm the shim's guarded import is clean.

## Follow‑ups (TODO)
- **Explore compiling cUR for the Pi Zero and fully replacing the pure‑Python `ur2/`
  library.** Build cUR as a second CPython C extension in `seedsigner-raspi-lvgl` (append an
  `Extension("uUR", sources=[cUR .c + uUR.c])` to `setup.py`'s `ext_modules`, reusing the
  existing armv6 cross‑build env + C‑flag handling — it's independent of the LVGL
  extension). With native cUR on **both** platforms, `helpers/ur2/` (and its dual‑decoder
  fallback) could be deleted entirely — collapsing to a single decoder, aligning with the
  "clean cutover, no hybrid" philosophy, and removing the native/pure‑Python maintenance
  burden. Open questions to resolve first: host/CI pytest would then need the native module
  built (or keep a minimal `ur2` shim used only by tests); confirm cUR builds clean under
  the K210‑free PC/armv6 path (bundled `src/sha256/sha256.c`); and decide whether the
  CPython binding uses `uUR.c` as‑is or a small CPython‑specific wrapper.
- **UR encoder:** switch the animated‑QR *display* path to cUR's `UREncoder` (already in
  `uUR.c`) once the submodule + binding are wired.

## Risks
- **ESP‑IDF mbedtls component mismatch** (5.5.x vs cUR's 6.x target) → fallback to bundled
  `src/sha256/sha256.c`.
- **cUR maturity** (young repo, ~53 commits) → pin+review a commit on a bot fork; on‑device
  parity‑test against `ur2`; mitigated by Krux production use + its own test‑vector suite.
- **Dual‑decoder maintenance** (native + pure‑Python kept behavior‑compatible) → accepted
  pattern (embit `secp256k1`).
- **QSTR/include leakage** from `uUR.c` → headers are simple; if it bloats the QSTR scan,
  apply the `camera_scanner` thin‑facade split.
