# embit PSBT-processing slowness on ESP32 — investigation handoff

> **Status: SCOPING NOTE for a follow-up session. Do NOT treat as settled.** Written
> 2026-07-05 with the data at hand; no profiling has been done yet. The point of this
> doc is to say *what we know* and *where the next session should look* — not to
> propose a fix.

## The observation

With the **native `secp256k1`** module in firmware, an end-to-end A/B on the P4 (Waveshare
LCD 4.3) measured the "post-scan PSBT processing" that SeedSigner's `PSBTParser.parse` does —
`embit PSBT.parse(raw)` + per-input/output BIP32 derivation of our key:

| fixture | derivations | native secp | pure-Python secp |
|---|---|---|---|
| `psbt_2of3_p2wsh_3in_normal` | 4 | ~1.0 s | ~14 s |
| `psbt_2of3_p2wsh_100in_normal` | 101 | **~30 s** | did not finish in 9+ min |

The **secp EC is no longer the bottleneck** on the native path: with native
`ec_pubkey_create` ≈ 3.8 ms, the ~3 heavy EC ops/derivation × 101 derivations ≈ **~1–2 s**
of that 30 s. **So ~28 s of the 30 s is NOT secp** — it's embit's pure-Python machinery +
hashing + interpreter overhead. That remaining time is what this note is about.

## What's (probably) eating the ~28 s — hypotheses, unverified

These are informed guesses from the code shape, **not measured**. The next session should
profile to attribute the time before acting on any of them.

1. **BIP32 CKD volume + no account-node caching (likely the biggest lever).**
   The A/B harness derives the **full path from `root` for every input**:
   `root.derive([48', 0', 0', 2', 0, i])` — 6 CKD levels each, so ~101 × 6 ≈ **~600 CKD
   steps**, each an HMAC-SHA512 + point/scalar ops + Python object churn. The account node
   (`m/48'/0'/0'/2'`) is identical for every input; deriving it **once** and then deriving
   only `[0, i]` per input (2 levels) would cut CKD to ~4 + 101×2 ≈ **~200 steps (~3×
   fewer)**. Check whether SeedSigner's real `PSBTParser` already caches this (the harness
   does not) — if not, that's a direct win independent of embit internals.

2. **The HMAC wrapper is pure-Python even though the hash is native.**
   SHA-512 itself is now native (mbedtls, via `_hashlib_ext`), but `hmac` is the
   micropython-lib **pure-Python** `hmac.py` (frozen). Every CKD level constructs HMAC
   objects, XOR-pads the key, and does inner/outer `.copy()`/`.update()`/`.digest()` in
   Python. At ~600 CKD steps that Python glue adds up. A native HMAC-SHA512 (mbedtls
   `mbedtls_md_hmac*`, already linked) exposed to Python — or letting embit call a native
   HMAC directly — could remove most of it.

3. **embit `PSBT.parse` of a 52 KB PSBT is pure-Python byte parsing.**
   Parsing 100 inputs' witness UTXOs, scripts, and bip32-derivation maps is Python
   stream-parsing over a large buffer with lots of small `bytes`/int object creation. This
   may be a meaningful fixed cost even before any derivation runs. Time `PSBT.parse(raw)`
   alone vs the derivation loop to split parse-cost from derive-cost.

4. **General MicroPython interpreter overhead.** embit is pure Python; iterating inputs,
   building `PublicKey`/`DerivationPath` objects, dict lookups, etc. is inherently slower on
   the MCU than on a Pi Zero. This is the residual after 1–3 and is the hardest to move.

## Where to look (next session)

- **`embit/bip32.py`** — `HDKey.derive` / `HDKey.child`: does it cache derived nodes? Is
  there an API to derive an account xpub once and index cheaply from it? (Mitigation #1.)
- **`embit/psbt.py`** — `PSBT.parse` / `InputScope`/`OutputScope` parsing (Mitigation #3).
- **`.../lib/micropython-lib/python-stdlib/hmac/hmac.py`** (frozen) — the pure-Python HMAC
  wrapper; and `bindings/modhashlibext.c` / `deps/esp-hashlib-ext/` where a native HMAC-SHA512
  could be added alongside the sha512 type (Mitigation #2).
- **SeedSigner `PSBTParser._parse_inputs` / `_parse_outputs`** — the real derivation pattern
  (per-input path, caching) vs the harness's naive full-path-per-input.

## How to reproduce / measure

- Harness: `tools`-style scratch script `psbt_ab_timing.py` (this session's scratchpad) —
  pushes a PSBT to the device, times `PSBT.parse + per-matching-derivation` native vs a
  pure-Python-secp baseline (rebinds `secp256k1` in every loaded embit module to
  `embit.util.py_secp256k1`). **Note its non-caching full-path derivation (Mitigation #1).**
  To attribute the 30 s, split the timed block into `PSBT.parse` vs the derive loop, and
  count HMAC-SHA512 calls per derivation.
- Fixtures: `btc-datagen/output/psbt_2of3_p2wsh_{3,100}in_normal_psbt.txt` (base64), seed =
  fixture `alice` (2of3_p2wsh cosigner, fp `814d5ff8`); device BIP39 seed can be injected
  host-side (`hashlib.pbkdf2_hmac`) or computed on-device now that `hashlib.pbkdf2_hmac`
  exists.
- Prereq firmware: the native-secp + `_hashlib_ext` (SHA-512/PBKDF2) build from this session
  (P4). See `docs/knowledge/native-secp256k1-static-ecmult-required.md` and
  `docs/knowledge/hashlib-sha512-pbkdf2-mbedtls.md`.

## Explicitly out of scope of the note

No profiling, no embit patching, no HMAC-native spike was done here — deliberately. Start the
follow-up by **measuring** (parse-vs-derive split + HMAC-call count) before choosing a lever.
