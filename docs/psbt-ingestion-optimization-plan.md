# PSBT-ingestion performance — deep dive & optimization plan

> **Status: APPROVED PLAN — HIGH PRIORITY, DEFERRED. Not started.** Written 2026-07-05 as the
> actionable follow-up to `docs/embit-psbt-processing-slowness-investigation.md`. This document is the
> plan of record; implementation is scheduled for a **separate, later effort**. No firmware, embit, or
> SeedSigner **optimization** code has been written yet — but a TEMP Phase-0 timing harness now lives
> inline in `psbt_parser.py` and one guardrail was refined; see **the 2026-07-07 update at the bottom**.

## Context

With the native `secp256k1` module in firmware, the EC math is no longer the PSBT bottleneck
(native `ec_pubkey_create` ≈ 3.8 ms; all EC ops for a 100-input PSBT ≈ 1–2 s). Yet an on-device
A/B still measured **~30 s** to process the `psbt_2of3_p2wsh_100in_normal` fixture. The residual
~28 s is embit's pure-Python machinery + hashing glue + interpreter overhead. This plan pins down
**exactly where** that time goes and recommends how to cut it, with dedicated native C modules.

This code path is **security-sensitive** (BIP32 key derivation, PSBT ownership/change decisions).
The guiding principle throughout: **never hand-roll crypto; only swap leaf primitives for
audited-library (mbedtls) C, and keep all derivation/PSBT *orchestration* in reviewed Python.**

### Key correction that reframes the whole investigation

The `~30 s` number came from the scratch harness `psbt_ab_timing.py`, whose workload does **not
match production**. The harness derives the **full path from the master root** for the one matching
(alice) derivation per input. The real `PSBTParser` (`seedsigner/src/seedsigner/models/psbt_parser.py`)
instead derives **all 3 cosigners × 2 levels** from the PSBT's depth-4 global xpub (`_get_cosigners`,
psbt_parser.py:340).

The raw CKD-level count is ~600 either way (100×1×6 ≈ 100×3×2 — a coincidence), so the ~30 s
ballpark holds. But the real path carries extra pure-Python weight the harness omits (3× object
churn, `psbt.tx` rebuild churn, ~100 wasted `root.child(0)` derivations — see Findings). So the real
path is likely **somewhat more** work, and its phase distribution differs. **Attribution must be
measured against the real `PSBTParser.parse()`, not the harness**, before committing native effort.

---

## Findings — what the code actually does (verified)

### The real per-input hot loop (multisig, the fixture case)
- `PSBTParser.parse()` (psbt_parser.py:71) → `_set_root()` (one `HDKey.from_seed`, cached) →
  `_fill_missing_fingerprints()` → `_parse_inputs()` → `_parse_outputs()`.
- `_parse_inputs` (psbt_parser.py:96) calls `_get_policy` → `_get_cosigners` **for every input**.
  `_get_cosigners` derives **each of the 3 cosigner pubkeys** via `xpub.derive(der.derivation[-2:])`
  = 2 public-CKD levels → **~600 CKD levels for 100 inputs**, plus a per-input `_parse_multisig`
  (3× `ec.PublicKey.parse`) and a `to_base58()` sort. This validation genuinely differs per input
  (leaf pubkeys differ) — it is not blindly skippable.
- `_parse_outputs` (psbt_parser.py:114) re-runs `_get_policy`/`_get_cosigners` per output; multisig
  **change detection is script-reconstruction + byte-compare (no CKD)**.

### Per-CKD-level cost in embit (`embit/bip32.py` `child`, line 184)
Every level pays three pure-Python leaf costs on top of the now-native EC/SHA:
1. **HMAC-SHA512 wrapper is pure-Python.** The frozen micropython-lib `hmac.py` (hmac.py:41-42) runs
   two 128-iteration Python XOR-pad loops + 2 hash-object allocs per call. The SHA-512 *digest* is
   native; the HMAC *construction* is not. embit uses the **one-shot**
   `hmac.new(cc, data, "sha512").digest()` — no incremental/copy — so a native HMAC-SHA512 fully
   covers it.
2. **RIPEMD-160 is pure-Python.** `hash160` (fingerprint, once per level) = native SHA-256 +
   **pure-Python RIPEMD-160** (`embit/hashes.py:6-13`), because `MBEDTLS_RIPEMD160_C` is disabled and
   MicroPython's `hashlib` has no `ripemd160`/`new()`.
3. **base58 encode on every node.** `HDKey.__init__` (bip32.py:45-47) calls `to_base58()` (base58 +
   double-SHA-256) purely to sanity-check the version prefix — on every derived node.

### Two non-crypto inefficiencies (safe to fix, no key logic)
4. **`psbt.tx` rebuild churn.** `embit/psbt.py` `PSBT.tx` (psbt.py:664-671) is a property that rebuilds
   the entire `Transaction` (all 100 `TransactionInput`s) on **every access**; `_parse_outputs` reads
   `self.psbt.tx.vout[i]` many times.
5. **~100 wasted derivations.** `_fill_scope` (psbt_parser.py:436) computes `self.root.child(0)` (a full
   private CKD step) once **per input and per output**, even when no fingerprint is missing (the normal
   fixture). Crucially, `HDKey.child()` sets a child's `.fingerprint` to `hash160(parent.sec())[:4]`, so
   `self.root.child(0).fingerprint` is simply the **master fingerprint** — the exact value embit already
   exposes cached and CKD-free as `self.root.my_fingerprint` (bip32.py:63-68). So the CKD is pure waste:
   ~100+ HMAC-SHA512 derivations to fetch a value available for free. (This is the code behind the
   recently-added missing-fingerprint quality-of-life fix — see the feature-preservation guardrail.)

### Native infra already in place (the template to extend)
`_hashlib_ext` (module) = `bindings/modhashlibext.c` binding + `deps/esp-hashlib-ext/` mbedtls component
+ frozen `deps/third-party/hashlib.py` shim. `hashlib_ext.c` **already includes `mbedtls/md.h`** (full
`mbedtls_md_hmac*` API) and `MBEDTLS_MD_C` is enabled — so a native HMAC needs **zero new dependencies**.
Build wiring: `bindings/micropython.cmake` (source + `__idf_esp-hashlib-ext` link),
`scripts/build_firmware.sh` (`MICROPY_EXTRA_DIRS`), the board `manifest.py` (freezes the shim).

### Upstream status — none of these optimizations exist yet (checked 2026-07-05)
- **embit** (`diybitcoinhardware/embit` + the deployed `kdmukai/embit` fork): upstream `master` is 25
  commits past the pinned `v0.8.0`; the fork adds 14 more. The only `bip32.py`/`psbt.py` changes are
  taproot features + a reproducibility fix — **no caching**. Grep for `cache`/`memo`/`lru` in `bip32.py`
  across every ref (master, `kdmukai/profiling`, `kdmukai/micropython`, HEAD) is empty; `HDKey.derive`
  has no memoization anywhere. `kdmukai/profiling` is test scaffolding only. No open PR touches the
  CKD/parse hot path (closest: #117 legacy-PSBT parse *correctness*, #135 secp fallback).
- **SeedSigner** (`SeedSigner/seedsigner`): the local `psbt_parser.py` is **identical to `upstream/dev`**
  on every hot method (only diff: a `List`→`list` typing tweak). `self.root.child(0)` (line 437),
  per-input `_get_cosigners` re-derivation (line 341), and the `psbt.tx` churn are all present in current
  `dev`, unoptimized, with no PR addressing them. Note **PR #936** adds a regression test for
  `_parse_outputs()` change detection — a guardrail item (a) must keep green.

So all the wins below are **net-new** — nothing is being duplicated.

---

## Recommended plan (phased)

### Phase 0 — Corrected measurement (do first; gates everything else)
Rebuild the profiler around the **real** `PSBTParser.parse()` (not the harness workload) and attribute
the ~30 s before writing any C:
- Instrument sub-phases: `PSBT.parse(raw)` alone, `_fill_missing_fingerprints`, `_parse_inputs`
  (policy+cosigner derivation), `_parse_outputs`. Time each with `time.ticks_ms`.
- Add counters for **HMAC-SHA512 calls** and **RIPEMD-160 calls** (wrap `hmac.new` and
  `embit.hashes.ripemd160` with a counting shim for the measurement run only).
- Run native-secp vs pure-Python-secp on `psbt_2of3_p2wsh_{3,100}in_normal` (seed = alice, fp
  `814d5ff8`). Reuse the existing device-injection method (host-computed BIP39 seed).
- Deliverable: a table attributing the ~30 s to {parse, fill, input-derive, output-derive} and the
  measured HMAC/RIPEMD counts. This decides how much each Phase-1/2 lever is worth.

### Phase 1 — Native leaf primitives via mbedtls (primary native work)
Both are audited-library primitives added to the **builder repo only** (no embit/seedsigner edits),
validated against published vectors **and** the existing pure-Python impl byte-for-byte on-device.

1. **Native HMAC-SHA512** in `_hashlib_ext`.
   - Add `hlx_hmac_sha512(...)` to `esp-hashlib-ext` (mbedtls `mbedtls_md_hmac*`), mirroring the
     existing `hlx_pbkdf2_sha512`. Expose in `modhashlibext.c` as an incremental HMAC type
     (`update`/`digest`/`copy`, matching the `sha512` type) — the faithful drop-in — with a one-shot
     `hmac_sha512(key, msg)` as the minimal alternative.
   - Route embit to it via a **builder-owned frozen `hmac.py`** that shadows micropython-lib's `hmac`:
     for `digestmod` sha512 → native; **all other digestmods fall back to the reference pure-Python
     `HMAC`** (keep the current file as the fallback). Preserves the full `hmac` API.
   - Validate: RFC 4231 HMAC-SHA512 vectors + byte-identical vs current pure-Python wrapper over the
     fixture's CKD inputs.

2. **Native RIPEMD-160** (makes `hash160` fully native, zero embit change).
   - **Second consumer (higher-value than parse):** brute-force address verification calls `hash160`
     **2–4× per index** in a tight loop (native segwit = 2/index, nested segwit = 4/index), so native
     RIPEMD-160 pays off even more there than in PSBT parse. See
     `docs/signing-and-address-verification-review.md`.
   - Enable `CONFIG_MBEDTLS_RIPEMD160_C=y` in the board `sdkconfig.board`.
   - Add `ripemd160` to `esp-hashlib-ext` + `modhashlibext.c`, and add a `new(name, data=b"")` factory
     to the frozen `hashlib.py` shim mapping `"ripemd160"` → native. embit's `hashlib.new("ripemd160")`
     (hashes.py:6) then succeeds and auto-selects native — **no embit patch**.
   - Validate: standard RIPEMD-160 vectors + byte-identical vs `embit.util.py_ripemd160` on the
     fixture's `hash160` inputs.

### Phase 2 — Python-side algorithmic fixes — **SeedSigner side only**
All three edits are in the **seedsigner** repo (`src/seedsigner/models/psbt_parser.py`) — the
`kdmukai/embit` fork stays pristine and upstream-trackable (embit root-cause work is deferred, below).
Behavior-preserving; each validated by byte-identical parse output vs the pre-change baseline across all
btc-datagen fixtures. Items (a)/(b) are non-crypto and safe; (c) touches derivation and needs the most
scrutiny. Production business logic (SeedSigner upstream) → highest review bar; commit trailer per repo
rule (no Claude trailer for SeedSigner-owned upstream).

- **(a) Hoist `self.psbt.tx`** in `_parse_outputs` — assign `tx = self.psbt.tx` once and index `tx`,
  eliminating repeated full-`Transaction` rebuilds. Pure refactor, zero crypto risk. Must keep PR #936's
  change-detection regression test green.
- **(b) Fix `_fill_missing_fingerprints` — WITHOUT weakening the missing-fingerprint feature.** Replace
  `self.root.child(0).fingerprint` with `self.root.my_fingerprint`: it yields the **byte-identical**
  master fingerprint that gets written into the filled `DerivationPath`, but with no CKD derivation
  (cached `hash160`). **The feature's logic is untouched** — the zero-fingerprint detection
  (`fingerprint == b"\x00\x00\x00\x00"`) and the ownership-proving `self.root.derive(...)` + `.sec()`
  compare are unchanged; we only stop re-deriving a value we can read for free. Optionally also skip a
  scope with no zero-fingerprint derivations (behavior-identical: those items already return `None`).
- **(c) Memoize the change-branch node in `_get_cosigners`** — cache `xpub.child(change)` keyed by
  `(xpub, change)` across inputs (change ∈ {0,1}), turning per-cosigner 2-level derives into 1 level +
  a one-time ~6-node cache. ~2× fewer CKD levels. Same math, memoized. Requires careful cache-key
  correctness (must not cross xpubs) and byte-identical validation. Local per-parse cache only — no
  change to embit's `HDKey` semantics.

*(The base58-per-node cost in `HDKey.__init__` — finding #3 — has no SeedSigner-side workaround and is
therefore deferred to the embit to-do below, not in this phase.)*

### Phase 3 — NOT recommended now (documented for completeness)
A native BIP32-CKD C module would embed key-tweak/secret-key logic in hand-written C — high risk for
modest additional gain once Phases 1–2 land (EC + HMAC + RIPEMD already native). Revisit only if Phase-0
data shows the residual is still dominated by CKD glue *after* Phases 1–2. Likewise, a native PSBT
byte-parser is a large surface for uncertain benefit; defer unless Phase 0 shows `PSBT.parse` alone
dominates.

---

## TO-DO (deferred — separate effort): embit efficiency + upstream PRs
We are intentionally **not** touching the embit fork in this effort. Logged as **high-priority but
deferred** root-cause efficiency improvements to `kdmukai/embit`, to be implemented and **PR'd upstream
to `diybitcoinhardware/embit`** later (no Claude trailer per the embit-ecosystem rule; keep the fork
upstream-trackable):
- **Cache `HDKey` child nodes** — add memoization to `HDKey.child`/`derive` so repeated derivations of
  the same index reuse the node (root cause behind Phase-2c; benefits all embit consumers).
- **Cache the `PSBT.tx` property** — stop rebuilding the entire `Transaction` on every access (root
  cause behind Phase-2a; the SeedSigner hoist is only a local workaround).
- **Drop the redundant base58 version-check in `HDKey.__init__`** — it runs a base58 encode +
  double-SHA-256 on every derived node just to sanity-check the version prefix (finding #3). This is the
  one win with **no** SeedSigner-side workaround; it needs an embit change.

Each must be validated against embit's own BIP32 test vectors before any PR.

---

## Security guardrails (apply to all phases)
- **No hand-rolled crypto.** HMAC-SHA512 and RIPEMD-160 come from **mbedtls** — the same trust boundary
  already accepted for the native SHA-256/SHA-512/PBKDF2.
- **Keep orchestration in reviewed Python.** BIP32 CKD tweak logic, PSBT semantics, and ownership/change
  decisions stay in embit/SeedSigner. Only leaf primitives move to audited C.
- **Differential validation on-device.** Every native primitive validated against (1) published test
  vectors and (2) the existing pure-Python implementation byte-for-byte, over representative fixture
  inputs, before it is trusted. Pure-Python impls remain as reference/fallback.
- **Correctness gate = byte-identical parse output.** After each phase, `PSBTParser.parse()` must produce
  identical policy / change_data / amounts / fingerprints vs the pre-change baseline on every btc-datagen
  fixture.
- **Preserve the missing-fingerprint fix.** `_fill_missing_fingerprints` was added to detect ownership
  when a coordinator emits all-zero fingerprints — a real user quality-of-life fix. The (b) optimization
  must NOT alter that behavior: verify with an all-zero-fingerprint PSBT fixture that ownership detection
  and the filled fingerprint value are unchanged before/after. Add such a fixture to the Phase-0/Phase-2
  test set if one is not already present (SeedSigner PR #936 covers the change path; the zero-fingerprint
  path needs its own guard).
- **Coordination:** if another session is editing this repo, confirm no overlap on `modhashlibext.c`,
  `esp-hashlib-ext`, `micropython.cmake`, `build_firmware.sh`, and the board `sdkconfig`/`manifest.py`
  before touching them.

## Files to modify
- **Native primitives (builder):** `deps/esp-hashlib-ext/{hashlib_ext.h,hashlib_ext.c}` (+ `hlx_hmac_sha512`,
  `hlx_ripemd160`); `bindings/modhashlibext.c` (HMAC type + `ripemd160`); `deps/third-party/hashlib.py`
  (add `new()` factory); a builder-owned frozen `hmac.py` + freeze it in the board `manifest.py`;
  `.../boards/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/sdkconfig.board` (`CONFIG_MBEDTLS_RIPEMD160_C=y`).
- **Python-side (seedsigner, this effort):** `src/seedsigner/models/psbt_parser.py` only —
  `_parse_outputs` `psbt.tx` hoist; `_fill_missing_fingerprints` `child(0)`→`my_fingerprint`;
  `_get_cosigners` change-branch memoization. **No embit-fork edits** (deferred to the embit to-do).
- **Profiler:** a new scratch/`tools` script wrapping the real `PSBTParser.parse()` with sub-phase timers
  + HMAC/RIPEMD counters (Phase 0).

## Verification
1. **Phase 0:** run the corrected profiler on the 3-in and 100-in fixtures (native vs pure-Python secp);
   produce the attribution table + primitive counts.
2. **Phase 1:** on-device, RFC 4231 + RIPEMD-160 vectors pass; native HMAC/RIPEMD outputs byte-identical
   to pure-Python over fixture inputs; re-run the Phase-0 profiler and record the new 100-in wall-clock.
3. **Phase 2:** `PSBTParser.parse()` output byte-identical to baseline across all btc-datagen fixtures;
   re-profile; record the final 100-in wall-clock.
4. Build/flash via `/esp-build`; measure on the P4 (Waveshare LCD 4.3), the primary release target.

---

## 2026-07-07 update — Phase-0 partial (inline timers) + guardrail refinement

Session on the seedsigner side, prompted by the user observing a **multi-second "Parsing PSBT…"
spinner** after seed selection (before the Review Transaction screen) on the P4 with native secp now
in firmware. Confirmed the spinner wraps exactly `PSBTParser.__init__ → parse()` — the loading screen
is fired just before construction in `PSBTOverviewView` (`seedsigner/src/seedsigner/views/psbt_views.py:92-99`).
This all corroborates the plan above; the deltas below refine it.

### Phase 0 was started inline (diverges from the "separate scratch profiler" approach)
Rather than a standalone profiler script, TEMP sub-phase timers were added **directly inside
`PSBTParser.parse()`** (`seedsigner/src/seedsigner/models/psbt_parser.py`), all marked `# ── TEMP ──`
for clean removal:
- A CPython/MicroPython timing shim (`time.ticks_ms`/`ticks_diff`, monotonic fallback) mirroring the
  `hardware/scan_consumer.py` idiom.
- One `logger.debug` line, tagged `[PSBT-TIMING]`, emitting a self-describing breakdown:
  `inputs=… outputs=… multisig=… | total=…ms set_root=… fill_fp=… parse_inputs=… parse_outputs=…`.
  Maps directly onto the plan's attribution buckets ({parse-ish/set_root, fill, input-derive,
  output-derive}).
- **Emitted at `debug`** — below the default INFO threshold, so whoever captures the numbers must lower
  the log level to DEBUG for `seedsigner.models.psbt_parser` (P4 firmware side owns log enablement/
  monitoring). Measurements always run; only the emission is gated.

**Still TODO for a complete Phase 0** (unchanged from the plan): the **HMAC-SHA512 and RIPEMD-160 call
counters** (wrap `hmac.new` / `embit.hashes.ripemd160` with a counting shim for the measurement run),
and running the native-vs-pure-Python-secp A/B. The inline timers give the wall-clock split but not the
primitive-call attribution that decides Phase-1 value. **Remove the inline TEMP block once data is
collected** (it lives in production business logic).

### Guardrail refinement for Phase 2(b)/(c): gate on presence-of-zeros, NOT `is_multisig`
Per user domain knowledge, the **all-zero-fingerprint degenerate case only occurs with single-sig**
(multisig can't be assembled without each cosigner's key-origin, so those fields are always populated).
Consequence: on a normal multisig PSBT `_fill_missing_fingerprints` rescues nothing, so its per-scope
`root.child(0)` is 100% waste there — reinforcing Phase 2(b).

**Design constraint:** the optional "skip a scope with no zero-fingerprint derivations" shortcut must gate
on the **actual presence of an all-zero fingerprint**, never on `is_multisig`. Gating on `is_multisig`
would silently skip the rescue if a degenerate multisig PSBT ever appeared, and — because SeedSigner
won't sign an input whose fingerprint doesn't match the seed — that would render such a PSBT
**unsignable**. Presence-of-zeros is behavior-preserving in every case (single-sig with real
fingerprints, multisig, and the degenerate single-sig path where the rescue fires). This just sharpens
the existing "Preserve the missing-fingerprint fix" guardrail; no change to the plan's `child(0) →
my_fingerprint` recommendation.

### A small interactive fixture for quick before/after
Alongside the 100-in stress fixture, use a **10-input 2-of-3 p2wsh consolidation → 1 output + fee** as a
fast, hand-drivable on-device check (it's the user's real test PSBT). Back-of-envelope base-point-mul
count for it: **≈77** = ~11 (`_fill_missing_fingerprints` `child(0)`, per input+output) + ~60
(`_parse_inputs` `_get_cosigners`, 3 cosigners × 2 levels × 10) + ~6 (`_parse_outputs` on the change
output). So on this fixture the fill-fingerprint lever (Phase 2b) is only ~14%, and cosigner
re-derivation (Phase 2c) is ~85% — the inline `[PSBT-TIMING]` split should confirm `parse_inputs`
dominating `fill_fp` before any code is changed.
