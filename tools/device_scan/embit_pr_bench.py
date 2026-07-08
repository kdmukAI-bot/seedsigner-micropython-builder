#!/usr/bin/env python3
"""On-device A/B of the embit Phase-3 PR (base58-per-node removal + HDKey.child memo).

Deploys each embit variant to /lib/embit, hard-resets for a clean heap, and runs the
raw BIP32-CKD micro-benchmark (embit_ckd_bench_dev.py) — isolating the embit change
from SeedSigner's app-side 2c cache. Prints per-variant timing + a correctness digest
(must match across variants) and the speedup. Restores the pinned embit afterward so
the device is left in the shipping state.

Robust against the P4 USB-CDC byte-drop (docs/knowledge/deploy-serial-truncation.md):
deploys via deploy_app's proven push+verify+retry loop. Only bip32.py differs between
master and master+PR, so after a full (force) deploy of the first variant, later
variants push non-force (size-match skip) → only the changed file re-transfers.

Run UNBUFFERED so progress is visible:
  python3 -u tools/device_scan/embit_pr_bench.py --n 9
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)

import _devenv  # noqa: E402
import deploy_app as da  # noqa: E402
from sd_format_push import raw_exec, hard_reset_and_wait  # noqa: E402

BENCH = os.path.join(HERE, "embit_ckd_bench_dev.py")
EMBIT_REPO = os.path.dirname(os.path.dirname(_devenv.EMBIT_SRC))


def log(*a):
    print(*a, flush=True)


def export_ref(ref):
    dst = os.path.join(tempfile.gettempdir(), "embit_export_%s" % ref.replace("/", "_"))
    src = os.path.join(dst, "src", "embit")
    if not os.path.isdir(src):
        os.makedirs(dst, exist_ok=True)
        tar = subprocess.run(["git", "-C", EMBIT_REPO, "archive", ref],
                             check=True, capture_output=True).stdout
        subprocess.run(["tar", "-x", "-C", dst], input=tar, check=True)
    return src


def deploy_embit(port, embit_src, mpy_cross, force):
    """Push embit (+ the bench module) with deploy_app's retry/verify loop."""
    ser = hard_reset_and_wait(port, do_reset=False)
    try:
        raw_exec(ser, da.DEVICE_HELPERS)
        ok = False
        for attempt in range(1, da.PUSH_ATTEMPTS + 1):
            expected = {}
            on_dev = da.device_tree(ser)
            f = force and attempt == 1
            try:
                da.push_tree(ser, embit_src, da.EMBIT_DST, "embit", {"__pycache__"},
                             (".pyc",), expected, on_dev, f,
                             compile_mpy=True, mpy_cross=mpy_cross)
                ok = da.verify(ser, expected)
            except RuntimeError as e:
                log("[embit] push error %d/%d: %s" % (attempt, da.PUSH_ATTEMPTS, e))
            if ok:
                break
            log("[embit] re-pushing missing/mismatched (attempt %d) ..." % (attempt + 1))
        if not ok:
            sys.exit("[embit] verify FAILED after %d attempts" % da.PUSH_ATTEMPTS)
        da.push_bytes(ser, open(BENCH, "rb").read(), "/lib/embit_ckd_bench_dev.py")
    finally:
        ser.close()


def run_bench(port, n):
    ser = hard_reset_and_wait(port, do_reset=True)   # clean heap
    try:
        out = raw_exec(ser, "import embit_ckd_bench_dev as B\nB.run(%d)\n" % n, timeout=120)
        line = next((l for l in out.splitlines() if l.startswith("EMBITBENCH")), None)
        return line, out
    finally:
        ser.close()


def field(line, key):
    for tok in line.split():
        if tok.startswith(key + "="):
            return tok.split("=", 1)[1]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--n", type=int, default=9)
    ap.add_argument("--variant", action="append", default=[], help="label=path (repeatable)")
    ap.add_argument("--restore-ref", default="v0.8.0", help="embit ref to redeploy at the end; '' to skip")
    args = ap.parse_args()

    scratch = os.environ.get("SCRATCH", "/tmp/claude-1000/-home-kdmukai-dev-seedsigner-micropython-builder/f736b263-b796-4705-9fe2-f5c04c03bcf2/scratchpad")
    variants = args.variant or [
        "master=%s/embit-master/src/embit" % scratch,
        "master+PR=%s/embit-phase3/src/embit" % scratch,
    ]
    mpy_cross = da.resolve_mpy_cross(None)

    results = []
    for i, v in enumerate(variants):
        label, path = v.split("=", 1)
        if not os.path.isdir(path):
            sys.exit("[embit] variant path missing: %s" % path)
        log("\n==== variant: %s (%s) ====" % (label, path))
        deploy_embit(args.port, path, mpy_cross, force=(i == 0))  # full-deploy first, then skip-unchanged
        line, out = run_bench(args.port, args.n)
        if not line:
            log("[FAIL %s]\n%s" % (label, out)); continue
        log(line)
        results.append((label, int(field(line, "total_ms_med")), field(line, "digest")))

    log("\n==== EMBIT PR on-device impact (raw BIP32-CKD workload, ESP32-P4) ====")
    digs = {d for _, _, d in results if d}
    log("correctness: %s" % ("BYTE-IDENTICAL across variants"
                             if len(digs) == 1 else "MISMATCH! %s" % digs))
    for label, ms, _ in results:
        log("  %-12s %6d ms" % (label, ms))
    if len(results) == 2:
        log("  speedup: %.2fx (%d -> %d ms)"
            % (results[0][1] / results[1][1], results[0][1], results[1][1]))

    if args.restore_ref:
        log("\n[restore] redeploying pinned embit %s ..." % args.restore_ref)
        deploy_embit(args.port, export_ref(args.restore_ref), mpy_cross, force=True)
        log("[restore] done; device back on shipping embit.")


if __name__ == "__main__":
    main()
