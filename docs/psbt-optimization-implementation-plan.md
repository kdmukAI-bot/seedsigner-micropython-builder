# PSBT-ingestion optimization — step-by-step implementation plan

> **Status: COMMITTED — BUILD ALL PHASES. Master execution plan for a fresh session. Not started (2026-07-08).**
> **Decisions (user, 2026-07-08):**
> 1. **Build every speedup step.** Do NOT gate on impact measurements or stop to decide whether we've
>    "gone far enough" — all four phases are built as one program of work. Phase-0 instrumentation stays,
>    but purely for before/after data + regression tracking, **not** as a go/no-go gate.
> 2. **Prepare the embit PR, but DO NOT open it.** Build the embit changes on a branch, make the
>    commits, run the validation, and draft the PR body — then **stop**. The human opens the PR. No
>    `gh pr create` for embit.
> 3. **One session owns everything.** A single Claude session drives all three repos (builder firmware
>    + seedsigner app + embit-prep) — centralized organization/control, not split across sessions.
> 4. **This doc is the execution plan.** Written to be run from a **fresh context** — see
>    *Execution notes for a fresh session* at the bottom before starting.
> This is the *how*. The *why* / analysis / attribution model lives in
> [`docs/psbt-ingestion-optimization-plan.md`](psbt-ingestion-optimization-plan.md)
> and [`docs/embit-psbt-processing-slowness-investigation.md`](embit-psbt-processing-slowness-investigation.md).
> Target symptom: the **~7 s "Parsing PSBT…" spinner** after seed selection on the P4
> (Waveshare LCD 4.3) for the 10-input 2-of-3 p2wsh consolidation PSBT; the ~30 s
> 100-input case is the stress fixture. Modeled result after all changes: **~7 s → ~2–3 s.**

## Scope — three repos, and one deliberate decision

| Repo | Upstream owner | What changes here | Commit trailer |
|---|---|---|---|
| `seedsigner-micropython-builder` (this) | `kdmukai` | Native HMAC-SHA512 + RIPEMD-160 primitives; frozen `hmac.py`/`hashlib.py` shims; board `sdkconfig` | **both** (kdmukai + Claude) |
| `seedsigner` | `SeedSigner` org | `psbt_parser.py` algorithmic fixes (2a/2b/2c) | **no Claude trailer** |
| `embit` (`/home/kdmukai/dev/embit`) | `diybitcoinhardware` | base58-per-node removal **+ `HDKey` child-node cache + `PSBT.tx` cache** → **prepare PR (branch + commits + drafted body); DO NOT open** | **no Claude trailer** |

**The embit build & the pinning boundary.** The two big levers — native HMAC-SHA512 and native
RIPEMD-160 — work against the **pinned `embit==0.8.0`** *unchanged*, because embit calls
`hashlib.new("ripemd160")` (`hashes.py:6,9`) and `hmac.new(key, data, digestmod="sha512")`
(`bip32.py:53,202`), both of which we reroute to native via the **frozen shims** — no embit edit.
The embit source changes (Phase 3) are the **root-cause fixes** — base58-per-node removal
(`bip32.py:45-48`, the one with no app-side workaround), plus `HDKey` child-node caching and
`PSBT.tx` caching (the upstream versions of 2c/2a). **We build all of them and PR them upstream to
`diybitcoinhardware/embit`** (no Claude trailer).
- **Firmware embit pin stays `embit==0.8.0`** (standing user preference: keep the signer's embit an
  upstream-trackable, audited pinned release). **Consequence:** the Phase-3 embit speedups —
  chiefly the base58-per-node removal — are **PR'd upstream but do NOT run on our device** until the
  pin is bumped. Phases 1–2 already carry the ~7 s → ~2.3 s win on-device *without* that pin change;
  the base58 lever was the smallest slice anyway.
- **One-line flip if you want it live on-device:** bump the app's `requirements.txt` embit pin (or
  vendor the patched embit in the deploy) once the change is merged/released upstream. Called out as
  an explicit, always-available option — not a decision this plan blocks on.

## Guiding constraints (all phases)
- **No hand-rolled crypto.** HMAC-SHA512 / RIPEMD-160 come from **mbedtls** — same trust boundary
  already accepted for native SHA-256/SHA-512/PBKDF2 in `_hashlib_ext`.
- **Correctness gate = byte-identical.** Every native primitive validated against (1) published
  vectors and (2) the existing pure-Python impl byte-for-byte over fixture inputs. Every Python
  change validated by byte-identical `PSBTParser.parse()` output vs the pre-change baseline across
  all btc-datagen fixtures. Pure-Python impls stay as reference/fallback.
- **Preserve the missing-fingerprint fix** (see 2b). Gate any skip on **presence-of-zeros**, never
  on `is_multisig`.
- **Measure before/after each phase** on the 10-in and 100-in fixtures (seed = fixture `alice`,
  fp `814d5ff8`) using the inline `[PSBT-TIMING]` line + primitive counters (Phase 0).
- **Coordination:** another session edits this repo. Before touching `modhashlibext.c`,
  `deps/esp-hashlib-ext/*`, `bindings/micropython.cmake`, `scripts/build_firmware.sh`, and the
  board `sdkconfig.board`/`manifest.py`, confirm no overlap.

---

## Phase 0 — Instrument first (before/after data + regression tracking; NOT a gate)

All phases are being built regardless of what the numbers say — Phase 0 exists so every step has a
clean before/after and so any correctness/perf **regression** is caught, not so we can decide
whether to continue. Do it first anyway: it establishes the baseline and the per-step deltas.

The inline sub-phase timers already exist in `seedsigner/src/seedsigner/models/psbt_parser.py`
(`# ── TEMP ──` block at lines 15-29 and 97-128; emits `[PSBT-TIMING] … total set_root fill_fp
parse_inputs parse_outputs` at `logger.debug`). What's missing is the **primitive-call
attribution** (records where the time goes; does not change which phases we build).

**0.1** Add a TEMP counting shim (same `# ── TEMP ──` discipline, remove with the timers) that wraps
- `embit.bip32.hmac.new` (count HMAC-SHA512 constructions), and
- `embit.hashes.ripemd160` (count RIPEMD-160 calls),
resetting per `parse()` and appending `hmac=… ripemd=…` to the `[PSBT-TIMING]` line.

**0.2** Lower the log level for `seedsigner.models.psbt_parser` to DEBUG (firmware side owns log
enablement) and capture the line for **both** fixtures, **native-secp vs pure-Python-secp**
(rebind `secp256k1` → `embit.util.py_secp256k1` for the baseline, per the A/B harness).

**0.3 Deliverable:** an attribution table — `{set_root, fill_fp, parse_inputs, parse_outputs}` ms +
`{hmac, ripemd}` counts — for 10-in and 100-in, captured as the **baseline**. Re-run after each
phase to record the delta (regression tripwire). The model predicts `parse_inputs` ≈ 78 % of CKD
work (~77 CKD on the 10-in fixture); if the split differs, that's informational for future tuning —
it does **not** change the build-everything decision.

> Baseline number to beat: the 10-in consolidation ≈ **7 s**.

---

## Phase 1 — Builder: native leaf primitives (biggest flat win; no embit/seedsigner edit)

Both extend the existing `_hashlib_ext` component + `modhashlibext.c` binding + frozen shims. The
template is the current SHA-512 type — mirror it exactly (inline opaque mbedtls ctx in the GC
object, `block_size`/`digest_size` locals, plain-C API in `hashlib_ext.h` so no mbedtls headers
reach the usermod QSTR scan).

### 1.1 — Native HMAC-SHA512
Files: `deps/esp-hashlib-ext/hashlib_ext.{h,c}`, `bindings/modhashlibext.c`, a **new builder-owned
frozen `hmac.py`** (`deps/third-party/hmac.py`), board `manifest.py`.

1. **Plain-C API** — add to `hashlib_ext.h`/`.c`, backed by mbedtls `mbedtls_md_hmac*` (`md.h` is
   already included; `MBEDTLS_MD_C` already enabled — **zero new deps**):
   - incremental: `hlx_hmac_sha512_ctx_size/init(key,len)/update/digest/clone/free` (mirror the
     sha512 type — the faithful drop-in), and
   - one-shot: `hlx_hmac_sha512(key, klen, msg, mlen, out[64])` (embit uses the one-shot
     `hmac.new(...).digest()` form, so this covers the hot path).
2. **Binding** (`modhashlibext.c`) — expose an `hmac_sha512` incremental type
   (`update`/`digest`/`copy`) plus a module-level one-shot `hmac_sha512(key, msg)`; register both in
   `hashlib_ext_globals_table`.
3. **Frozen `hmac.py` shim** (`deps/third-party/hmac.py`) that **shadows** micropython-lib's
   pure-Python `hmac`:
   - `digestmod == "sha512"` (embit's call) → native one-shot / native incremental type;
   - **all other digestmods → the reference pure-Python `HMAC`** (copy micropython-lib's current
     `hmac.py` in as the fallback body). Preserves the full `hmac` API.
4. **Freeze it** — in the board `manifest.py`, **replace `require("hmac")`** (currently line 10,
   pulls the pure-Python lib) with `module("hmac.py", base_path="$(MPY_DIR)/../../../deps/third-party")`
   (same pattern as the existing `hashlib.py` freeze at line 26). Avoids a duplicate-`hmac` name clash.
5. **Validate:** RFC 4231 HMAC-SHA512 vectors; byte-identical vs the pure-Python wrapper over the
   fixture's CKD inputs; on-device smoke that `PSBTParser.parse()` output is byte-identical.

### 1.2 — Native RIPEMD-160 (makes `hash160` fully native; **auto-picked by embit**)
Files: `deps/esp-hashlib-ext/hashlib_ext.{h,c}`, `bindings/modhashlibext.c`,
`deps/third-party/hashlib.py`, board `sdkconfig.board`.

1. **Enable mbedtls RIPEMD-160** — add `CONFIG_MBEDTLS_RIPEMD160_C=y` to
   `deps/micropython/mods/new_files/ports/esp32/boards/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/sdkconfig.board`
   (currently absent — RIPEMD-160 is disabled by default).
2. **Plain-C API + binding** — add `hlx_ripemd160(msg, len, out[20])` (mbedtls `mbedtls_ripemd160`)
   and a `ripemd160` hash type in `modhashlibext.c` (mirror the sha512 type: `update`/`digest`/`copy`).
3. **`new()` factory in the frozen `hashlib.py`** — add
   `def new(name, data=b""): return ripemd160(data) if name == "ripemd160" else <builtin ...>`.
   Then **embit's `hashlib.new("ripemd160")` (`hashes.py:6,9`) auto-selects native — no embit patch**;
   its `try/except` fallback to `util.py_ripemd160` stays intact for host/CPython.
4. **Validate:** standard RIPEMD-160 vectors; byte-identical vs `embit.util.py_ripemd160` over the
   fixture's `hash160` inputs. (Second, higher-value consumer: address-verification calls `hash160`
   2–4×/index — see `docs/signing-and-address-verification-review.md`.)

**End of Phase 1:** rebuild/flash (P4), re-run the Phase-0 profiler, record the new 10-in and 100-in
wall-clock. Expectation: ~2× on its own.

---

## Phase 2 — SeedSigner: algorithmic fixes (`psbt_parser.py` only; embit stays pristine)

All three are in `seedsigner/src/seedsigner/models/psbt_parser.py`. Behavior-preserving; each
validated by byte-identical parse output vs the pre-change baseline on every btc-datagen fixture.
Production business logic → **highest review bar; no Claude trailer**.

### 2a — Hoist `self.psbt.tx` in `_parse_outputs` (safe, no crypto)
`_parse_outputs` (line 151) reads `self.psbt.tx.vout[i]` **~12×** in the loop (lines 159, 207, 220,
223, 226, 228, 231, 250, 254, 257, 259, 260). `PSBT.tx` is a **property that rebuilds the entire
`Transaction`** (all inputs+outputs) on **every access** (`embit/psbt.py:664-671`).
- **Change:** assign `tx = self.psbt.tx` once before the loop; index `tx.vout[i]` throughout.
  (Optionally hoist `tx.vout` too.) Pure refactor.
- **Guard:** keep SeedSigner **PR #936**'s `_parse_outputs` change-detection regression test green.

### 2b — `_fill_missing_fingerprints`: `child(0)` → `my_fingerprint` (kill wasted CKD, keep the fix)
`_fill_scope` (line 471) computes `signing_seed_fingerprint = self.root.child(0).fingerprint`
(line 473) **once per input and per output** — a full private CKD each time (~11 on the 10-in
fixture), 100 % waste on a normal PSBT.
- **Why it's byte-identical:** `HDKey.child(0)` sets the child's `.fingerprint` to
  `hashes.hash160(parent.sec())[:4]` (`bip32.py:196-218`), i.e. **the master fingerprint** — exactly
  what `self.root.my_fingerprint` returns (cached `hash160`, CKD-free; `bip32.py:63-68`).
- **Change:** compute `signing_seed_fingerprint = self.root.my_fingerprint` **once**, hoisted out of
  the per-scope `_fill_scope` closure (it's seed-constant).
- **Optional skip (must be presence-of-zeros gated):** only run a scope's fill if that scope actually
  contains a `b"\x00\x00\x00\x00"` fingerprint. **Never gate on `is_multisig`** — the all-zero case is
  single-sig-only, and gating on multisig would make a degenerate multisig PSBT unsignable. The
  ownership logic (`derived_key.key.sec() == public_key.sec()`, lines 486-488) is untouched.
- **Extra guard:** add an all-zero-fingerprint single-sig fixture and assert ownership detection +
  the filled fingerprint value are byte-identical before/after (PR #936 covers the change path, not
  the zero-fp path).

### 2c — Memoize the change-branch node in `_get_cosigners` (halves the dominant bucket)
`_get_cosigners` (line 363) does `xpub.derive(der.derivation[-2:])` (line 377) = **2 public-CKD
levels per cosigner per input** (3 × 2 × 10 = ~60 CKD on the 10-in fixture — the dominant bucket).
`der.derivation[-2:]` = `[change, index]`, so `xpub.derive([change, index]) =
xpub.child(change).child(index)`; the first level depends only on `(xpub, change)` (change ∈ {0,1}).
- **Change:** cache `xpub.child(change)` keyed by `(xpub, change)` across inputs, then derive only
  `.child(index)` per input → ~1 level/input + a one-time ~6-node cache (**~2× fewer CKD**).
  `_get_cosigners` is currently a `@staticmethod`; convert to an instance method (or thread a cache
  dict from `parse()`) so the cache lives at **per-parse scope**.
- **Cache-key correctness (critical):** key on the **xpub object identity** so the cache never
  crosses xpubs; cache is per-`parse()` only — **no change to embit's `HDKey` semantics**. Validate
  byte-identical cosigner sets.

**End of Phase 2:** re-run the profiler; `PSBTParser.parse()` byte-identical to baseline across all
fixtures; record the final 10-in and 100-in wall-clock.

---

## Phase 3 — embit: build the root-cause fixes + PR upstream

Build **all three** embit source changes and PR them to **`diybitcoinhardware/embit`**. These are
the root causes behind the app-side workarounds (2a/2c) plus the one win that has no workaround
(base58). Each validated against embit's own BIP32 test vectors before the PR.

### 3.1 — Drop the per-node base58 version-check (the no-workaround win)
`HDKey.__init__` runs `self.to_base58()[1:4] != "prv"/"pub"` on **every constructed node**
(`bip32.py:45-48`) — a base58 encode + double-SHA-256 purely to sanity-check the version prefix.
- **Change:** replace with a direct membership check of `self.version` against the known
  xprv/xpub (and testnet/nested) version-byte constants — no base58 encode.

### 3.2 — Cache `HDKey` child nodes (upstream version of 2c)
Memoize `HDKey.child`/`derive` so repeated derivations of the same index reuse the node. Benefits
all embit consumers (2c is only the SeedSigner-local per-parse version of this).

### 3.3 — Cache the `PSBT.tx` property (upstream version of 2a)
`PSBT.tx` (`psbt.py:664-671`) rebuilds the entire `Transaction` on every access. Cache it (invalidate
on input/output mutation). 2a is only the SeedSigner-local hoist workaround for the same cost.

### 3.4 — PR mechanics — **PREPARE, DO NOT OPEN**
- Branch on `/home/kdmukai/dev/embit` (fork `kdmukai/embit`); the eventual PR targets
  **`diybitcoinhardware/embit`**.
- **No Claude trailer** (embit-ecosystem rule); keep the fork upstream-trackable.
- Split into reviewable commits (base58 / child-cache / tx-cache) — they touch different concerns
  and the caching ones carry more review surface than the base58 fix.
- **Validate** against embit's BIP32 test vectors and **draft the PR body** (summary, per-commit
  rationale, vector results) — then **STOP**. Do **not** run `gh pr create`; leave the branch pushed
  (or local) and the drafted body in the repo/handoff so the human can open the PR when ready.

### 3.5 — Firmware pin stays `embit==0.8.0` (so these land upstream, not on our device yet)
The app keeps its pinned, audited `embit==0.8.0`; **we do not vendor a patched embit into the build.**
Phases 1–2 already deliver the ~7 s → ~2.3 s win on-device without it. The Phase-3 embit speedups are
therefore an **upstream contribution** whose on-device benefit lands only when the pin is later bumped
(a one-line `requirements.txt` change, available anytime the change is merged/released). This is a
standing preference, not an impact gate — Phase 3 is still built in full.

---

## Cross-cutting

- **Measurement harness:** reuse the existing device-injection A/B (host-computed BIP39 seed);
  fixtures `btc-datagen/output/psbt_2of3_p2wsh_{3,10,100}in_normal_psbt.txt`. The 10-in
  consolidation is the fast hand-drivable check; 100-in is the stress case.
- **Remove the TEMP instrumentation** (`# ── TEMP ──` timers + the Phase-0 primitive counters) from
  `psbt_parser.py` once the final before/after data is captured — it lives in production business logic.
- **Build/flash:** `/esp-build`; measure on the **P4 (Waveshare LCD 4.3)**, the primary release target.
- **Trailers per repo:** builder = both; seedsigner = no Claude; embit = no Claude.

## Sequencing & dependencies (all four phases are being built)
```
Phase 0 (instrument)  ── baseline + per-step regression tracking (NOT a gate)
        │
        ├─▶ Phase 1  (builder native HMAC + RIPEMD)     ← independent of Phase 2, biggest flat win
        │         │  works with pinned embit 0.8.0, no embit/seedsigner edit
        │         ▼   re-measure (record delta)
        ├─▶ Phase 2  (seedsigner 2a/2b/2c)              ← independent of Phase 1; may land in parallel
        │         │  byte-identical parse output gate
        │         ▼   re-measure (record delta)
        └─▶ Phase 3  (embit base58 + child-cache + tx-cache) ──▶ PREPARE PR, DO NOT OPEN (no Claude trailer)
                     firmware stays pinned 0.8.0 → lands upstream, not on-device until a pin bump
```
Phase 0 goes first (baseline). Phases 1 and 2 are independent and may proceed in either order / in
parallel; each is re-measured so contributions are attributable and regressions caught. Phase 3 is
decoupled; it is **built in full** and its PR is **prepared but left unopened**, and it reaches the
device only if/when the pin is later bumped. **No phase is skipped or gated on the numbers.**

## Expected outcome (model; measured for real in Phase 0 / after each step)
| After | 10-in spinner |
|---|---|
| now | ~7.0 s |
| + 2b | ~6.2 s |
| + 2c | ~4.3 s |
| + native HMAC+RIPEMD (Phase 1) | **~2.3 s** |
| + Phase-3 base58 (only if pin bumped) | slightly lower; small slice |
Floor with these levers ≈ ~1.5–2 s (irreducible pure-Python interpreter/object churn in embit,
not addressed here). A native CKD/PSBT module is **explicitly not recommended** — see the analysis doc.

## Testing & validation automation (build the harness alongside the code)
Two lanes; between them, **all correctness validation is automatable with deterministic pass/fail**,
and perf capture is automatable (only the read-out is for humans). Build these as each phase lands.
- **Host pytest (no hardware) — authoritative for correctness:**
  - **Phase 2 byte-identical parse output** across all `btc-datagen` fixtures (platform-independent
    Python) — extend `seedsigner/tests/test_psbt_parser.py` (`run_basic_test`). Add the **PR #936**
    change-detection guard, a **zero-fingerprint single-sig** fixture (2b / issue #359), and a
    **cache-key** test (2c must not cross xpubs).
  - **Phase 3 embit** vs embit's own BIP32 test vectors.
- **Device REPL harness (P4 serial) — parity + real timing + mem/crash:** extend
  `tools/device_scan/psbt_ab_timing.py` (already does the native↔pure-Python-secp rebind) into a
  one-command `RESULT … PASS/FAIL` harness that injects the host-computed seed, reads the fixture
  **from /sd** (don't paste the 52 KB 100-in over serial), runs `parse()` N times, and emits a
  machine-readable line:
  `RESULT fixture=… total_ms=… set_root=… fill_fp=… parse_inputs=… parse_outputs=… hmac=… ripemd=… parse_digest=<sha256> mem_free_delta=… PASS`.
  - **`parse_digest`** = sha256 of the canonicalized parse result (policy + change_data + amounts +
    fingerprints + destinations); compare to the host baseline → makes "byte-identical" an
    automatable on-device assertion.
  - **Phase 1 primitives:** RFC 4231 HMAC-SHA512 + standard RIPEMD-160 vectors, and byte-identical
    native-vs-`embit.util.py_ripemd160` / pure-Python-HMAC over fixture inputs — all deterministic
    pass/fail from the REPL. `hashlib.new("ripemd160")` succeeding *is* the `CONFIG_MBEDTLS_RIPEMD160_C`
    + routing proof.
  - **Robustness:** stash timing/counters on the parser instance (e.g. `self._timing`) so the REPL
    reads an attribute instead of scraping logs; keep all instrumentation on the Python `print`/attr
    path (never C `ESP_LOG` — raw-REPL nulls it; use paste-mode if any esp_log is involved).
- **Genuinely manual (not REPL):** Docker **build + flash** (`/esp-build`) before each device round;
  one **end-to-end UI pass** per phase (scan → select seed → spinner tears down → Review Transaction
  renders) — `parse()` timing is a faithful proxy but the LVGL flow wants one human look.

---

## Execution notes for a fresh session (READ FIRST)

This plan is meant to be executed by **one session from a fresh context**, owning all three repos.
Orient, then work top-to-bottom.

**Repos (siblings under `/home/kdmukai/dev/`):**
- `seedsigner-micropython-builder/` (this repo) — Phase 0 counters wiring check, Phase 1 native
  primitives, the device-REPL harness (`tools/device_scan/`), builds/flash.
- `seedsigner/` — Phase 0 inline instrumentation (already present, uncommitted) + Phase 2 fixes;
  companion doc `docs/_integration/psbt-parse-performance-todo.md` (indexed in `_integration/INDEX.md`).
- `embit/` (fork `kdmukai/embit`) — Phase 3 changes; **prepare PR, do not open.**

**Companion doc:** the SeedSigner-side spec is self-contained at
`seedsigner/docs/_integration/psbt-parse-performance-todo.md` — read it before Phase 2.

**Suggested order:** Phase 0 (finish counters + capture baseline) → Phase 1 (native HMAC, then
RIPEMD; build/flash/validate) → Phase 2 (2a → 2b → 2c; host pytest + device parity) → Phase 3 (embit
base58 → child-cache → tx-cache; validate vectors; **draft PR body, stop**). Re-measure after each.

**Git / commit / PR discipline (per repo CLAUDE.md rules):**
- **Nothing is committed or pushed without explicit user permission**, via the hook-gated
  `bash ~/.claude/hooks/authorize-git N` flow. `cd` into the target repo and run `git` there (never
  `git -C`).
- **Trailers:** builder → **both** (kdmukai + Claude); seedsigner → **kdmukai only** (SeedSigner-org
  upstream); embit → **kdmukai only** (diybitcoinhardware ecosystem).
- **embit: PREPARE ONLY.** Commit on the fork branch + draft the PR body; **do NOT** `gh pr create`.
- **SeedSigner branch routing:** the `psbt_parser.py` edits are production business logic — invoke the
  **`seedsigner-stack`** skill for correct stack/branch placement rather than committing ad hoc.

**Build/flash:** use the **`/esp-build`** skill; validate on the **P4 (Waveshare LCD 4.3)**.

**Coordination:** confirm no other session is editing this repo's `modhashlibext.c`,
`deps/esp-hashlib-ext/*`, `bindings/micropython.cmake`, `scripts/build_firmware.sh`, board
`sdkconfig.board`/`manifest.py` before Phase 1.

**Cleanup:** remove the `# ── TEMP ──` instrumentation block from `seedsigner/psbt_parser.py` once the
final before/after data is captured (it lives in production business logic).
