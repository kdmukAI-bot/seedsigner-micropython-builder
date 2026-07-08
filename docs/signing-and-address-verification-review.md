# Signing & brute-force address verification — performance review (post-native-secp)

> **Status: RESEARCH FINDINGS — no code changed.** Written 2026-07-05 as a companion to
> `docs/psbt-ingestion-optimization-plan.md`. Reviews two SeedSigner flows that were **not** the focus
> of that plan — transaction signing and brute-force address verification — to check for residual
> dependencies / performance concerns now that native `secp256k1` + mbedtls SHA-512/PBKDF2 are in
> firmware. All file:line anchors verified against the working trees on this date.

## Scope

1. **Transaction signing** at the end of the PSBT review flow (`PSBTFinalizeView` → `PSBT.sign_with`).
2. **Brute-force address verification** ("Verify address" / scan-an-address → derive & match).

Both share one crucial architectural fact and one residual gap; they then diverge on their specific
concerns.

---

## The shared fact: native secp is auto-wired; RIPEMD-160 is the last pure-Python primitive

embit selects its secp backend **at import time** (`embit/misc.py:5`):

```python
if sys.implementation.name == "micropython":
    import secp256k1            # native C module (this firmware)
else:
    from .util import secp256k1 # CPython ctypes binding (Pi Zero)
```

Every EC operation in embit (`ec.py`, `bip32.py`) routes through this shim, so **native secp is wired
into signing AND address derivation with zero SeedSigner-side changes.** The native binding
(`deps/esp-secp256k1/mpy/modsecp256k1.c`, also the standalone `kdmukAI-bot/esp-secp256k1`) covers the
full contract, verified including:

- **Low-R grinding works natively.** `ecdsa_sign(msg32, secret32, None, ndata)` forwards `ndata` to
  `secp256k1_ecdsa_sign` (`modsecp256k1.c:376`), so embit's grind loop (`ec.py:218`, re-sign while DER
  > 70 bytes with a counter as extra nonce data) functions unchanged.
- **Side-channel hygiene is present.** The module context is `SIGN|VERIFY` and blinded with
  `secp256k1_context_randomize` seeded from `esp_fill_random()` on first use (`modsecp256k1.c:50`).

**The only remaining pure-Python crypto primitive on either path is RIPEMD-160** (`hash160`). SHA-256 is
the native built-in; SHA-512 / HMAC-SHA512 are native via `_hashlib_ext` + the frozen `hmac` module;
EC is native. `embit/hashes.py:6` probes `hashlib.new("ripemd160")`, which fails on this firmware (the
frozen `hashlib.py` shim exports only `sha512`/`pbkdf2_hmac` on top of `uhashlib`), so embit falls back
to `embit/util/py_ripemd160.py` — pure Python. This is the same finding #2 as the PSBT-ingestion plan;
**both flows below are additional consumers of that already-logged native-RIPEMD-160 to-do (PSBT plan
Phase 1.2).**

---

## Flow 1 — Transaction signing (`PSBTFinalizeView` → `PSBT.sign_with`)

**Path.** `PSBTFinalizeView.run()` (`seedsigner/src/seedsigner/views/psbt_views.py:527`) calls
`psbt.sign_with(psbt_parser.root)` **synchronously**, right after the "Approve" tap. embit's
`PSBT.sign_with` (`embit/src/embit/psbt.py:933`) loops every input and, per matching key:
`root.derive(path)` (BIP32 CKD) → `self.sighash(i)` (SHA-256) → `prv.sign(h)` → native `ecdsa_sign` +
low-R grind.

**Crypto status.** EC native; sighash SHA-256 native; BIP32 HMAC-SHA512 native-backed; grinding native.
`hash160` (RIPEMD-160) is used lightly — once per `sign_with` (`pkh = hash160(sec)`) — so it is
**negligible for signing**.

**Concerns.**
- **UI freeze.** `sign_with` runs on the UI thread with no loading screen. Native secp shrinks the
  stall dramatically, but a large multi-input / multisig PSBT can still visibly freeze the UI. Fix is
  app-side: wrap signing in a worker + loading screen (the `loading_screen` binding already exists).
- **Redundant re-derivation.** `sign_with` re-derives every input's key **from master**; parsing already
  derived those exact keys, and there is no HDKey child cache — so the work is repeated. Same root cause
  as PSBT plan Phase-2c / the embit child-cache to-do.
- **Not a concern:** embit *does* cache the segwit sighash midstates (`hash_prevouts` / `hash_sequence` /
  `hash_outputs`, `embit/src/embit/transaction.py:169`), so sighash is **not** O(n²) across inputs.

---

## Flow 2 — Brute-force address verification (`BruteForceAddressVerificationThread`)

**Path.** `SeedAddressVerificationView` spawns `BruteForceAddressVerificationThread`
(`seedsigner/src/seedsigner/views/seed_views.py:2002`), a **background** thread. It grinds indices from
0 upward; per index it computes **both** the receive and change address and compares each to the scanned
target (`get_single_sig_address` / `get_multisig_address`,
`seedsigner/src/seedsigner/helpers/embit_utils.py:69` / `:89`). The account-level `xpub` is derived
once at thread init (`seed_views.py:1999`); only the two soft levels repeat per index.

**Threading works on-device.** `BaseThread` routes through `seedsigner.compat.threading`, which maps
`Thread` → `_thread.start_new_thread` on MicroPython. So — unlike signing — the UI is **not** frozen;
this is a *throughput* concern (indices/sec → how long to reach a high index), not a UI block.

**Per-address crypto** (`embit/src/embit/script.py`), doubled per index (receive + change):

| Script type            | EC public CKD levels | RIPEMD-160 (`hash160`) | address encode          |
|------------------------|:--------------------:|:----------------------:|-------------------------|
| Native segwit (p2wpkh) | 2                    | **1**                  | bech32                  |
| Nested segwit (p2sh-p2wpkh) | 2               | **2**                  | base58check (2×SHA-256) |
| Legacy (p2pkh)         | 2                    | **1**                  | base58check             |
| Taproot (p2tr)         | 2 + tweak            | 0                      | bech32m                 |
| Multisig p2wsh (N-of-M)| **2·N**              | 0 (uses SHA-256)       | bech32                  |

So a native-segwit single-sig scan costs **4 public-CKD levels + 2 pure-Python RIPEMD-160 per index**;
nested segwit costs **4 RIPEMD-160 per index**.

**Concerns.**
1. **RIPEMD-160 pure-Python in the hot loop — the strongest case for native RIPEMD-160.** Single-sig
   native segwit = 2 RIPEMD-160/index; nested = 4/index; over hundreds/thousands of indices. Unlike PSBT
   parse (once per CKD level) or signing (once), this is a tight loop, so native mbedtls RIPEMD-160
   (PSBT plan Phase 1.2) yields its **biggest proportional win here**. This flow **raises the priority**
   of that already-logged to-do.
2. **Redundant branch re-derivation.** `xpub.derive([branch, index])` re-derives the `[0]`/`[1]` branch
   node from scratch every iteration. Hoisting the two branch nodes out of the loop (derive `xpub/0` and
   `xpub/1` once, then per-index only `branch.derive([index])`) cuts 4 CKD levels/index → 2 (halves the
   EC derivation). App-side (in the thread / `embit_utils`); analogous to PSBT plan Phase-2c but a
   different file. The embit HDKey child cache would achieve this automatically.
3. **Multisig scales with cosigner count** (2·N derivations/address); p2wsh uses SHA-256 (native), not
   RIPEMD-160, so it is purely EC-bound — already the biggest beneficiary of native secp.
4. **GIL time-slicing.** The background `_thread` and the UI/progress thread share the MicroPython GIL;
   the flat-out loop has no explicit yield, so raw throughput and UI responsiveness trade off.
5. **Validate native-secp stack usage inside the spawned `_thread`.** Signing runs on the main task;
   this is the main place native secp runs on a `_thread`-spawned stack. `USE_ECMULT_STATIC_PRECOMPUTATION`
   keeps secp's stack small (that is exactly why the runtime-gen build was rejected — see
   `docs/knowledge/native-secp256k1-static-ecmult-required.md`), so this is likely fine, but it is
   **untested in a spawned-thread context** — confirm no stack overflow on-device.
6. **Always starts at index 0.** No attempt to pre-seed an expected index from the address/QR metadata;
   "Skip 10" is the only manual jump. A high-index address is inherently linear-cost (not a crypto item).

---

## Priority to-do items

Ranked. Items marked *[shared]* already have a to-do of record in
`docs/psbt-ingestion-optimization-plan.md`; this session adds new justification, not a new task.

- **P1 — Native RIPEMD-160 via mbedtls.** *[shared — PSBT plan Phase 1.2, already tracked.]* This
  session adds a second, stronger consumer: brute-force address verification calls RIPEMD-160
  2–4×/index in a tight loop, so the win is larger here than in PSBT parse. No new work item — just
  weight the existing one accordingly. (Builder-only; reuses the `_hashlib_ext`/mbedtls path already in
  firmware; `CONFIG_MBEDTLS_RIPEMD160_C=y` + `new()` factory in the frozen `hashlib.py` shim.)
- **P2 — Hoist branch-node derivation in address verification.** *[new — seedsigner side.]* In
  `BruteForceAddressVerificationThread` / `embit_utils`, derive the receive/change branch nodes once and
  per-index derive only `[index]`. ~2× fewer CKD levels per index. No embit change.
- **P3 — Wrap signing in a loading screen / worker.** *[new — seedsigner side.]* `PSBTFinalizeView.run()`
  runs `sign_with` synchronously on the UI thread with no progress indicator. Use the existing
  `loading_screen` binding + a worker so large PSBTs don't freeze the UI.
- **P4 — HDKey child-node cache in embit.** *[shared — PSBT plan embit to-do, already tracked.]* Besides
  the PSBT-parse win, this also eliminates signing's redundant per-input re-derivation (Flow 1) and is
  the general fix behind P2. PR upstream to `diybitcoinhardware/embit` (no Claude trailer per repo rule).
- **P5 — Validate native-secp stack usage inside the spawned `_thread`** during address verification, on
  the P4. *[new — validation.]*

---

## Key references
- Signing view: `seedsigner/src/seedsigner/views/psbt_views.py:499` (`PSBTFinalizeView`), `:527` (`sign_with`).
- embit signing: `embit/src/embit/psbt.py:933` (`sign_with`), `embit/src/embit/ec.py:218` (`PrivateKey.sign` + grind).
- Backend dispatch: `embit/src/embit/misc.py:5`. RIPEMD-160 fallback: `embit/src/embit/hashes.py:6`.
- Native secp binding: `deps/esp-secp256k1/mpy/modsecp256k1.c:50` (ctx randomize), `:376` (`ecdsa_sign` + `ndata`).
- Address verification: `seedsigner/src/seedsigner/views/seed_views.py:1847` (view), `:2002` (thread loop);
  `seedsigner/src/seedsigner/helpers/embit_utils.py:69`/`:89` (address helpers);
  `embit/src/embit/script.py:128-148` (`p2pkh`/`p2sh`/`p2wpkh`/`p2wsh`/`p2tr`).
- Threading shim: `seedsigner/src/seedsigner/compat/threading.py`.
- Related: `docs/psbt-ingestion-optimization-plan.md`, `docs/knowledge/hashlib-sha512-pbkdf2-mbedtls.md`,
  `docs/knowledge/native-secp256k1-static-ecmult-required.md`.
