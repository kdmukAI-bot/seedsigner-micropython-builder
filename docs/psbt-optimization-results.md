# PSBT-ingestion optimization — measured results (running log)

Device: ESP32-P4 Waveshare LCD 4.3. Fixtures:
`tools/device_scan/fixtures/regtest_2of3_p2wsh_{3,10,100}in_xpubs.txt` (2-of-3
P2WSH, Sparrow-equivalent WITH global xpubs — see
`docs/knowledge/psbt-parse-cost-driven-by-global-xpubs.md`). Seed = alice
(814d5ff8), network REGTEST. Harness: `tools/device_scan/psbt_ab_timing.py`
(deploys instrumented app + pinned embit 0.8.0, runs `PSBTParser.parse()`).
`parse_digest` = 16-hex canonical hash of the parse result — the byte-identical
correctness anchor; MUST stay constant across every phase.

## Byte-identical anchors (host == device, verified)

| fixture | parse_digest | hmac | ripemd |
|---|---|---|---|
| 3-in  | `7ef23e29f5cbc5fb` | 29 | 28 |
| 10-in | `276e8273004a8f3c` | 78 | 77 |
| 100-in| `84acebb1ab3ae64c` | 708 | 707 |

## Phase 0 — baseline (current firmware: native secp, pure-Python HMAC + pure-Python RIPEMD-160)

Median of n=5 (3/10-in). Times in ms.

| fixture | total | set_root | fill_fp | parse_inputs | parse_outputs |
|---|---|---|---|---|---|
| 3-in  | 5325 | 24 | 676 | 3390 | 1284 |
| 10-in | **9531** | 15 | 1164 | **7558 (79%)** | 778 |
| 100-in| _(deferred — needs n=1 + longer timeout; byte-parse of 180 KB is slow)_ |

`parse_inputs` (the `_get_cosigners` 2-level CKD per cosigner per input) dominates
— confirms the analysis model. The residual per-CKD cost is pure-Python glue:
HMAC ipad/opad padding, the per-node base58 version check (`HDKey.__init__`),
RIPEMD-160 fingerprints, and object churn.

## Phase 1 — native HMAC-SHA512 + native RIPEMD-160 (firmware) ✅ DONE

Native primitives KAT-validated on device (RFC 4231 HMAC-SHA512 TC2; RIPEMD-160
`abc`/empty vectors); `hmac.new(digestmod="sha512")` → `_NativeHMACSHA512`;
`hashlib.new("ripemd160")` native. Median n=5.

| fixture | total | Δ vs baseline | set_root | fill_fp | parse_inputs | parse_outputs | digest |
|---|---|---|---|---|---|---|---|
| 3-in  | 1332 | **4.0×** | 17 | 129 | 865 | 325 | `7ef23e29f5cbc5fb` ✓ |
| 10-in | **2524** | **3.8×** | 13 | 268 | 1943 | 297 | `276e8273004a8f3c` ✓ |

**Far beyond the modeled ~2×** — the pure-Python HMAC ipad/opad padding + pure-Python
RIPEMD-160 dominated the residual, not just the hash compression. 10-in spinner
**9.5 s → 2.5 s**. `parse_inputs` (cosigner 2-level CKD) is still ~77 % — the Phase-2
`_get_cosigners` memo (2c) targets exactly that.

## Phase 2 — psbt_parser.py 2a/2b/2c (app; same firmware) ✅ DONE

2a `psbt.tx` hoist in `_parse_outputs`; 2b `child(0)`→`my_fingerprint` (hoisted,
CKD-free); 2c cosigner change-branch memo (per-parse `_ckd_cache`). All 15 existing
`test_psbt_parser.py` cases pass; every digest byte-identical. CKD counts:
10-in hmac 78→**37**, ripemd 77→**37**. Median n=7, **fresh boot**.

| fixture | total | Δ vs baseline | set_root | fill_fp | parse_inputs | parse_outputs | digest |
|---|---|---|---|---|---|---|---|
| 3-in  | 521  | 10.2× | 11 | 5 | 388  | 115 | `7ef23e29f5cbc5fb` ✓ |
| 10-in | **1291** | **7.4×** | 11 | 6 | 1085 | 201 | `276e8273004a8f3c` ✓ |
| 100-in| 14775 | — (P0 baseline was host-only) | 13 | 15 | 12759 | 146 | `84acebb1ab3ae64c` ✓ |

100-in stress case: `parse_inputs` is still 86 % — the residual is embit pure-Python
glue (per-node base58 version check + object churn), which is exactly what the
Phase-3 embit changes target (upstream-only; firmware stays 0.8.0).

2b drove `fill_fp` 268→6 (~44×); 2c drove `parse_inputs` 1943→1085 (~1.8×).

## Headline

**10-input 2-of-3 P2WSH "Parsing PSBT…" spinner: ~9.5 s → ~1.3 s (7.4×), byte-identical.**
Comfortably beats the plan's ~2.3 s model. Phase 1 (native primitives, firmware)
did the heavy lifting; Phase 2 (algorithmic, app) ~halved the remainder.

## Measurement note

Device parse time is sensitive to heap/GC state after a long REPL session — a
contaminated run showed the 10-in at 3963 ms with `set_root` (unchanged code)
inflated 13→24. `set_root` is the built-in device-speed sanity check; compare only
runs with comparable `set_root`. The harness now hard-resets before measuring
(`--reset`), and medians (not mins) are reported.

## Phase 3 — embit base58 / child-cache / tx-cache (upstream PR, NOT on device) — COMMITTED, PR NOT OPENED

Committed (`cdc8be5`) + pushed to the bot fork `kdmukAI-bot/embit` branch
`perf/psbt-parse-speedups` (base `master` == upstream); **PR not opened** (human opens
it to `diybitcoinhardware/embit`; no Claude trailer). Write-up + commit plan:
`docs/embit-phase3-pr-draft.md`.
- **3.1** base58-per-node version check → version-set membership (the no-workaround win).
- **3.2** memoize `HDKey.child`.
- **3.3** (`PSBT.tx` cache) **deferred** — unsafe to invalidate (inputs/outputs are
  plain lists); app-side 2a already captures the win on-device. Flagged for upstream
  design discussion.
- Validation: embit `tests/tests/` **96 passed** (incl. BIP32 vectors); host
  cosigner-derivation workload **9.03 ms → 1.98 ms (4.6×)**. Firmware stays embit 0.8.0.
- **On-device A/B (ESP32-P4, `tools/device_scan/embit_pr_bench.py`, raw BIP32-CKD
  workload, master vs master+PR, byte-identical digest `2997c32e`, median n=9):
  1561 ms → 401 ms = 3.89×.** Isolated from SeedSigner's app-side 2c cache — the PR's
  intrinsic value on constrained hardware (a consumer without change-branch caching
  gets the full 3.89×; SeedSigner-with-2c gets mostly the base58-removal portion
  incrementally). This is the data to attach when opening the PR.
  - ⚠ Harness note: the A/B *measurement* is reliable, but redeploying 1.6 MB of embit
    over the P4 USB-CDC can hang on a re-enumeration (the held serial handle stalls
    beneath `_read_until`'s deadline). Mitigations in `embit_pr_bench.py`: retry/verify
    loop + only the changed file re-pushes between variants. For the big restore push,
    wrap the deploy in a shell `timeout` and rerun (size-match skip resumes).

## Cross-board note

`bindings/modhashlibext.c` (the `_hashlib_ext` ripemd160 type) is compiled into
**every** board, so `CONFIG_MBEDTLS_RIPEMD160_C=y` was added to all four board
sdkconfigs (not just P4-43) to keep them link-clean. Only P4-43 has a `manifest.py`
freezing `hmac.py`/`hashlib.py`, so only P4-43 actually routes embit → native from
Python; the other three are bare dev boards (build-clean, no app).

## Landed state (2026-07-08)

- **builder** (this repo): branch `perf/psbt-parse-native-primitives` (native primitives
  firmware + this harness/docs), pushed origin/gitlab/codeberg, **PR #25 → kdmukai open**.
- **seedsigner**: the 2a/2b/2c change rides the mpy stack as the next contribution on the
  integration tip — `integration/lvgl-mpy == feat/language-selection == 4f59d5e` on all
  three mirrors (perf + the langpack session's work). No separate PR (the stack PRs later).
- **embit**: `cdc8be5` on `kdmukAI-bot/embit` `perf/psbt-parse-speedups` (bot fork); PR not opened.
- **Device**: P4 deployed with the tip app + pinned embit 0.8.0 (`import-smoke` PASS) and
  booted (`--mode run`) — SeedSigner UI live on the display.
