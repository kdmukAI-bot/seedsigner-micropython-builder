"""Device-side embit BIP32-CKD micro-benchmark (runs on MicroPython / ESP32-P4).

Isolates the embit Phase-3 PR (base58-per-node removal + HDKey.child memo) from
SeedSigner's app-side caching: it drives raw embit derivation, so no PSBTParser 2c
cache is in the way. Models the cosigner-resolution workload of a real multisig
parse — for each "parse" it re-parses N account xpubs from base58 (fresh nodes, so
the per-node HDKey.__init__ version check + child cache behave as within one parse)
and derives [change, index] for a spread of indices.

Reports median ms + a correctness digest (sha256 over the derived pubkeys) so the
two embit variants are proven byte-identical.
"""
import gc
import time
import hashlib
import binascii


def run(n=7, indices=11, accounts=3):
    from embit import bip32
    from embit.networks import NETWORKS

    seed = bytes((i * 7 + 1) & 0xFF for i in range(64))
    root = bip32.HDKey.from_seed(seed, version=NETWORKS["main"]["xprv"])
    # Account xpubs as base58 (re-parsed fresh each iteration).
    accts = [root.derive("m/48h/0h/%dh/2h" % a).to_public().to_base58()
             for a in range(accounts)]

    times = []
    digest_hex = None
    for _ in range(n):
        gc.collect()
        h = hashlib.sha256()
        t0 = time.ticks_ms()
        xpubs = [bip32.HDKey.from_base58(a) for a in accts]  # fresh nodes / "parse"
        for xp in xpubs:
            for idx in range(indices):
                h.update(xp.derive([0, idx]).key.sec())
        dt = time.ticks_diff(time.ticks_ms(), t0)
        times.append(dt)
        digest_hex = binascii.hexlify(h.digest()).decode()[:16]

    times.sort()
    med = times[len(times) // 2]
    n_ckd = accounts * indices * 2  # [change, index] = 2 CKD levels each
    print("EMBITBENCH accounts=%d indices=%d ckd_per_iter=%d n=%d "
          "total_ms_med=%d min=%d max=%d digest=%s free=%d"
          % (accounts, indices, n_ckd, n, med, times[0], times[-1],
             digest_hex, gc.mem_free()))
    return med
