# embit Phase-3 PR — PREPARED, DO NOT OPEN (draft for human review)

> **Status (2026-07-08):** changes implemented + validated on a worktree branch;
> **not committed, not pushed, PR not opened** (per the PSBT-optimization plan and
> repo git discipline). The human opens the PR when ready. **No Claude co-author
> trailer** (diybitcoinhardware ecosystem rule).

## What & where
- **Target upstream:** `diybitcoinhardware/embit`.
- **Base:** `master` == `upstream/master` == `8d75912` (verified in sync).
- **Branch (committed + pushed to the bot fork):** `perf/psbt-parse-speedups` on
  `origin` = `git@github.com:kdmukAI-bot/embit.git`. Review it with:
  ```
  cd /home/kdmukai/dev/embit
  git fetch origin perf/psbt-parse-speedups
  git log --oneline master..origin/perf/psbt-parse-speedups
  ```
  The PR is **NOT open** — the human opens it from a fork of `diybitcoinhardware/embit`
  when ready.

## Motivation
On the SeedSigner ESP32-P4 firmware, parsing a real multisig PSBT (Sparrow, with
the global-xpub map) is dominated by BIP32 CKD across the cosigner set. Profiling
the derivation hot path surfaced two embit-level costs paid on **every constructed
node**:
1. a base58 encode + double-SHA-256 purely to sanity-check the version prefix, and
2. recomputation of child nodes that repeat across PSBT inputs (shared change/receive branch).

## Changes (committed as one prep commit on the bot fork; split into the two below when opening the upstream PR if preferred)

### Commit 1 — `bip32: validate version against known constants, not base58 per node`
`HDKey.__init__` validated its version via `self.to_base58()[1:4] != "prv"/"pub"`,
i.e. a full base58 encode (big-int division loop) + `double_sha256` checksum on
**every** node — including every intermediate CKD node. Replaced with a set
membership test against the xprv/xpub-family version bytes collected once from
`NETWORKS` (`_PRV_VERSIONS` / `_PUB_VERSIONS`). Equivalent for all NETWORKS
versions (main/test/regtest/signet × x/y/z/Y/Z prv+pub). This is the win with **no
app-side workaround**.

### Commit 2 — `bip32: memoize HDKey.child derivations`
`HDKey.child(index)` recomputed the HMAC-SHA512 + EC tweak on every call. Added a
per-node `self._children` cache keyed by the normalized index. `child()` is
deterministic and returns a fresh node, so this is byte-identical; it reuses shared
prefixes (e.g. several inputs deriving under the same change branch of one xpub).

## NOT included — Commit 3 (PSBT.tx cache): deliberately deferred
The plan also listed caching the `PSBT.tx` property (rebuilds the whole
`Transaction` on every access). **Not implemented here** — embit exposes
`PSBT.inputs`/`PSBT.outputs` as plain lists with no mutation hook, so a cache
cannot be safely invalidated on external mutation without a larger design change
(wrapping the lists or making them change-tracking properties). Shipping a cache
that can go stale after a caller mutates an input/output would be a correctness
hazard. **Recommendation:** raise it as a separate design discussion, or land it
with the lists converted to invalidating properties. The SeedSigner-side workaround
(hoist `self.psbt.tx` once per parse — "2a") already captures this win locally
where the mutation surface is controlled, so on-device performance does not depend
on the upstream cache.

## Validation
- **Correctness (byte-identical):** embit unit suite `tests/tests/` — **96 passed**
  (includes `test_bip32.py` published BIP32 test vectors). Integration tests skipped
  (need `requests` + a live bitcoind).
- **Performance (host, CPython):** representative single-parse cosigner-derivation
  workload (3 account xpubs × 11 indices, fresh nodes per parse):
  **baseline 9.03 ms → modified 1.98 ms (4.6×).**
- **Performance (on-device, ESP32-P4 / MicroPython — `tools/device_scan/embit_pr_bench.py`):**
  same raw BIP32-CKD workload, `embit master` vs `embit master+PR`, fresh-boot heap,
  median n=9, byte-identical (digest `2997c32e` on both):
  **master 1561 ms → master+PR 401 ms = 3.89×.** This isolates the embit change from
  any consumer-side caching, so it's the PR's intrinsic value on constrained hardware.
  Attribution: both 3.1 (base58 removal, helps every node) and 3.2 (child memo — the
  11 indices share one change branch) contribute. NOTE for reviewers/SeedSigner: a
  consumer that already caches the change-branch itself (SeedSigner's app-side "2c")
  gets mostly the 3.1 portion incrementally; a consumer without such caching gets the
  full 3.89×. The SeedSigner firmware pin stays `embit==0.8.0`, so this lands upstream
  only (reaches the device on a later one-line pin bump).

## Firmware pin
Firmware stays `embit==0.8.0` — these changes are an **upstream contribution**, not
shipped to the device. They reach the device only if/when the app's `requirements.txt`
pin is later bumped (one line), independent of this PR.

## PR mechanics (for the human)
1. Reapply the patch (above), review, split into the 2 commits described.
2. Commit trailer: `Co-Authored-By: kdmukai <934746+kdmukai@users.noreply.github.com>`
   only — **no Claude trailer** (diybitcoinhardware ecosystem).
3. Push to `kdmukai/embit`, open PR → `diybitcoinhardware/embit`.
