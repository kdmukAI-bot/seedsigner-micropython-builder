#!/usr/bin/env python3
"""Generate byte-perfect, representative multisig-signing PSBT fixtures for the
PSBT-parse timing harness, from the live bitcoin-regtest node.

Why not btc-datagen or a bare `walletcreatefundedpsbt`? Both omit the PSBT global
xpub map (PSBT_GLOBAL_XPUB). A real coordinator (Sparrow) always includes it, and
SeedSigner's `_get_cosigners` resolves each input's pubkeys against it. WITHOUT the
global xpubs, `_get_cosigners` raises and the parse does ~6.5x LESS BIP32 CKD work
(the expensive 2-level `xpub.derive(...)` per cosigner per input never runs) — so a
baseline built on such a PSBT is not representative of the ~7 s device spinner.

This script:
  1. selects N UTXOs from the regtest `multisig` wallet,
  2. builds a consolidation PSBT via `walletcreatefundedpsbt` (byte-perfect, no
     paste corruption),
  3. injects the 3 cosigner account xpubs (from bitcoin-regtest/descriptors.txt)
     as the PSBT global xpub map — making it a faithful Sparrow-equivalent,
  4. writes the base64 to tools/device_scan/fixtures/.

Wallet: 2-of-3 P2WSH, cosigners alice/bob/carol, account m/48h/1h/0h/2h (regtest,
coin-type 1h). Seed to parse with = `alice` (fp 814d5ff8), network = REGTEST.

Run with the bitcoin-regtest venv (has embit):
  /home/kdmukai/dev/bitcoin-regtest/.venv/bin/python make_regtest_psbt.py [N ...]
"""
import json
import os
import subprocess
import sys

REGTEST = "/home/kdmukai/dev/bitcoin-regtest"
FIX_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")

# From bitcoin-regtest/descriptors.txt — account tpubs @ m/48h/1h/0h/2h.
COSIGNERS = [
    ("814d5ff8", "tpubDDy79wPtBb2cJgz5wbA85e8D44CgKBTaXLgcWzkGJq7LC6q9CNwYjaxgYUwE3cHLzEumLSpcwfDXWcbpHSZej4Gu6tgpNgZo5mU3mhY3wPw"),
    ("02aaa648", "tpubDEsUwSRxjWhhw7KsqhP2yLByx5rAL7eQCQ4oKb9gh9nTCNqMCNS5FPsAHtRA1SZDMSncdbceRuKu4LeNBUnX3jm4RCERNYyZcopJQQD6j24"),
    ("5142fc0d", "tpubDEMsWXv5UygyBqmt1gT1f8Ch9FDXKKWGmtsxVi3n7mkLXBofumfeW2neGdXZ7GpSY32oXxLwJPpjZiV36oQ9PkyGKo44VjkwxZ33D6ugrhp"),
]
H = 0x80000000
ACCOUNT_PATH = [48 | H, 1 | H, 0 | H, 2 | H]  # m/48h/1h/0h/2h (regtest)


def cli(*args):
    return subprocess.check_output(
        ["bash", os.path.join(REGTEST, "scripts/cli.sh"), "-rpcwallet=multisig", *args],
        cwd=REGTEST,
    ).decode()


def make(n: int):
    from embit.psbt import PSBT, DerivationPath
    from embit import bip32

    unspent = json.loads(cli("listunspent", "0", "9999999"))
    if len(unspent) < n:
        raise SystemExit(f"only {len(unspent)} UTXOs available, need {n}")
    sel = unspent[:n]
    inputs = [{"txid": u["txid"], "vout": u["vout"]} for u in sel]
    total = round(sum(u["amount"] for u in sel), 8)
    dest = cli("getnewaddress", "", "bech32").strip()
    options = {"add_inputs": False, "subtractFeeFromOutputs": [0], "fee_rate": 5}
    res = json.loads(cli("walletcreatefundedpsbt", json.dumps(inputs),
                         json.dumps([{dest: total}]), "0", json.dumps(options)))

    psbt = PSBT.from_base64(res["psbt"])
    for fp, tpub in COSIGNERS:
        psbt.xpubs[bip32.HDKey.from_base58(tpub)] = DerivationPath(bytes.fromhex(fp), ACCOUNT_PATH)

    os.makedirs(FIX_DIR, exist_ok=True)
    path = os.path.join(FIX_DIR, f"regtest_2of3_p2wsh_{n}in_xpubs.txt")
    with open(path, "w") as f:
        f.write(psbt.to_string() + "\n")
    print(f"[{n:>3}in] inputs={n} global_xpubs={len(psbt.xpubs)} "
          f"fee={res.get('fee')} -> {os.path.relpath(path)}")


if __name__ == "__main__":
    counts = [int(a) for a in sys.argv[1:]] or [3, 10, 100]
    for n in counts:
        make(n)
