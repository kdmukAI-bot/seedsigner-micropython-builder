# PSBT parse cost is driven by the global-xpub map — test fixtures without it under-measure ~6.5×

**Context:** measuring the "Parsing PSBT…" spinner on the ESP32-P4 (SeedSigner
firmware). Discovered 2026-07-08 while building the Phase-0 baseline for the
PSBT-ingestion optimization.

## The finding

For a 2-of-3 P2WSH multisig PSBT, the dominant cost in `PSBTParser.parse()` is the
BIP32 CKD work inside `_get_cosigners` — but **that code path only runs if the PSBT
carries the global xpub map** (`PSBT_GLOBAL_XPUB`, embit `psbt.xpubs`).

`_get_cosigners` (psbt_parser.py) resolves each input's script pubkeys against the
**global** xpub map:

```python
for xpub in xpubs:                       # psbt.xpubs — the GLOBAL map
    if origin_der.fingerprint == der.fingerprint:
        if origin_der.derivation == der.derivation[:-2]:
            if xpub.derive(der.derivation[-2:]).key == pubkey:   # 2 public CKD levels
                cosigners.append(xpub.to_base58()); break
```

If `psbt.xpubs` is **empty**, the inner loop never runs, `len(cosigners) != len(pubkeys)`
raises `RuntimeError("Can't get all cosigners")`, and `_get_policy`'s `try/except`
swallows it → the policy gets `{m, n}` but **no `cosigners`**, and the expensive
2-level `xpub.derive(...)` per cosigner per input **never executes**.

### Measured impact (10-input consolidation, host counts, platform-independent)

| PSBT | global xpubs | HMAC-SHA512 (CKD) | RIPEMD-160 | `_get_cosigners` |
|---|---|---|---|---|
| no global xpubs | 0 | **12** | 11 | raises → skipped |
| with global xpubs | 3 | **78** | 77 | runs (dominant) |

Same fixture, same seed — a **6.5× swing** in derivation work purely from the
presence of the global-xpub field. With it, `parse_inputs` is ~79 % of parse time
and matches the analysis model (~77 CKD for 10 inputs); without it, a baseline
would show the main optimization target (`_get_cosigners`, "2c") doing nothing.

## Why the obvious fixtures all miss it

- **btc-datagen** (`common/psbt.build_multisig_psbt`) sets per-input
  `witness_script` + `bip32_derivations` but never populates `psbt.xpubs`.
- **Bitcoin Core** `walletcreatefundedpsbt` (regtest `multisig` watch-only wallet)
  also omits `PSBT_GLOBAL_XPUB`.
- **Sparrow ALWAYS includes it** for multisig — which is why the real device
  spinner is slow and the synthetic fixtures looked deceptively cheap.

Change detection still works without the global map (it uses each output's own
`witness_script` + derivations), so a no-xpubs fixture *parses correctly* — it just
skips the costly cosigner-resolution path. Easy to miss.

## The fix (for representative fixtures)

Inject the account xpubs as the global map — a faithful Sparrow-equivalent. See
`tools/device_scan/make_regtest_psbt.py`: it builds a byte-perfect consolidation
PSBT from the live regtest node, then adds the 3 cosigner account tpubs (from
`bitcoin-regtest/descriptors.txt`) as `psbt.xpubs`. Fixtures live at
`tools/device_scan/fixtures/regtest_2of3_p2wsh_{3,10,100}in_xpubs.txt`; seed =
`alice` (fp 814d5ff8), network = REGTEST.

## Takeaway

When profiling / testing PSBT parsing for multisig, **use a coordinator-style PSBT
that includes the global xpub map** (or inject it). A PSBT missing it exercises a
different, much cheaper code path and will not reproduce the real signer workload.
