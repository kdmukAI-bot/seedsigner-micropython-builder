#!/usr/bin/env python3
"""On-device PSBT-parse timing harness for the P4 (Waveshare LCD 4.3).

Deploys the (instrumented) SeedSigner app + a PINNED embit 0.8.0 export, pushes
the representative fixtures to the internal-flash VFS, and runs
`PSBTParser.parse()` on each — reporting the instrumented sub-phase breakdown,
the HMAC-SHA512 / RIPEMD-160 CKD counts, and a byte-identical `digest`.

Why a pinned embit 0.8.0 export (not the dev fork or a `pip` embit)? The firmware
pin stays `embit==0.8.0` for the whole optimization effort (Phase-3 embit changes
are prepared upstream, not shipped). The dev fork tip differs in bip32.py/psbt.py,
so we `git archive` the tag into a temp dir and deploy THAT — reproducible and
matching the intended firmware embit. Override with --embit-src to validate a
patched embit.

Fixtures: tools/device_scan/fixtures/regtest_2of3_p2wsh_{3,10,100}in_xpubs.txt
(byte-perfect from the live regtest node, WITH the PSBT global-xpub map so
`_get_cosigners` runs — see make_regtest_psbt.py). Seed = alice; network = REGTEST.

Usage:
    python3 tools/device_scan/psbt_ab_timing.py [--port /dev/ttyACM0]
        [--skip-app] [--n 5] [--fixtures 3in,10in,100in] [--embit-ref v0.8.0]
        [--embit-src /path/to/embit/src/embit]
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

FIX_DIR = os.path.join(HERE, "fixtures")
DEV_FIX = "/psbt_fix"
DRIVER = os.path.join(HERE, "psbt_bench_dev.py")
EMBIT_REPO = os.path.dirname(os.path.dirname(_devenv.EMBIT_SRC))  # .../embit


def ensure_embit_export(ref):
    """git archive the embit tag into a stable temp dir; return its src/embit."""
    dst = os.path.join(tempfile.gettempdir(), "embit_export_%s" % ref.replace("/", "_"))
    src_embit = os.path.join(dst, "src", "embit")
    if os.path.isdir(src_embit):
        return src_embit
    os.makedirs(dst, exist_ok=True)
    tar = subprocess.run(["git", "-C", EMBIT_REPO, "archive", ref],
                         check=True, capture_output=True).stdout
    subprocess.run(["tar", "-x", "-C", dst], input=tar, check=True)
    if not os.path.isdir(src_embit):
        sys.exit("[embit] export missing src/embit for ref %s" % ref)
    return src_embit


def dev_size(ser, path):
    out = raw_exec(ser, "import os\ntry:\n print(os.stat('%s')[6])\nexcept OSError:\n print(-1)\n" % path)
    for line in out.splitlines():
        line = line.strip()
        if line.lstrip("-").isdigit():
            return int(line)
    return -1


def push_fixture(ser, local, remote):
    data = open(local, "rb").read()
    if dev_size(ser, remote) == len(data):
        print("[fix] up-to-date", remote)
        return
    da.push_bytes(ser, data, remote)
    print("[fix] pushed", remote, "(%d bytes)" % len(data))


def deploy(ser, args):
    raw_exec(ser, da.DEVICE_HELPERS)
    if not args.skip_app:
        embit_src = args.embit_src or ensure_embit_export(args.embit_ref)
        print("[deploy] seedsigner:", args.seedsigner_src)
        print("[deploy] embit:     ", embit_src)
        mpy_cross = da.resolve_mpy_cross(None)
        for attempt in range(1, da.PUSH_ATTEMPTS + 1):
            expected, on_dev = {}, da.device_tree(ser)
            try:
                da.push_tree(ser, args.seedsigner_src, da.SS_DST, "seedsigner",
                             {"__pycache__", "resources"}, (".pyc",), expected, on_dev,
                             False, compile_mpy=True, mpy_cross=mpy_cross)
                da.push_tree(ser, embit_src, da.EMBIT_DST, "embit", {"__pycache__"},
                             (".pyc",), expected, on_dev, False,
                             compile_mpy=True, mpy_cross=mpy_cross)
                if da.verify(ser, expected):
                    break
            except RuntimeError as e:
                print("[deploy] push error (%d/%d): %s" % (attempt, da.PUSH_ATTEMPTS, e))
            if attempt == da.PUSH_ATTEMPTS:
                sys.exit("[deploy] verify FAILED after %d attempts" % da.PUSH_ATTEMPTS)
        da.ensure_dev_deps(ser)
    # driver + fixtures
    da.push_bytes(ser, open(DRIVER, "rb").read(), "/lib/psbt_bench_dev.py")
    raw_exec(ser, "_mkdirp('%s')" % DEV_FIX)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--skip-app", action="store_true", help="assume app+embit already on /lib")
    ap.add_argument("--n", type=int, default=5, help="parse() iterations per fixture")
    ap.add_argument("--fixtures", default="3in,10in,100in")
    ap.add_argument("--embit-ref", default="v0.8.0")
    ap.add_argument("--embit-src", default=None, help="override: deploy this embit src (e.g. patched)")
    ap.add_argument("--seedsigner-src", default=da.SS_SRC)
    ap.add_argument("--reset", action="store_true", help="hard machine.reset() before opening REPL")
    ap.add_argument("--run-timeout", type=int, default=180,
                    help="per-fixture raw_exec timeout (s); raise for the 100-in stress case")
    args = ap.parse_args()

    fixtures = []
    for tag in args.fixtures.split(","):
        tag = tag.strip()
        path = os.path.join(FIX_DIR, "regtest_2of3_p2wsh_%s_xpubs.txt" % tag)
        if not os.path.exists(path):
            sys.exit("[fix] missing fixture: %s" % path)
        fixtures.append((tag, path))

    print("[repl] opening %s (reset=%s) ..." % (args.port, args.reset))
    ser = hard_reset_and_wait(args.port, do_reset=args.reset)
    try:
        deploy(ser, args)
        for tag, path in fixtures:
            remote = "%s/%s" % (DEV_FIX, os.path.basename(path))
            push_fixture(ser, path, remote)
        print("\n==== RESULTS (embit %s, native secp) ====" % (args.embit_src or args.embit_ref))
        for tag, path in fixtures:
            remote = "%s/%s" % (DEV_FIX, os.path.basename(path))
            out = raw_exec(ser,
                           "import psbt_bench_dev as B\nB.run('%s', %d)\n" % (remote, args.n),
                           timeout=args.run_timeout)
            line = next((l for l in out.splitlines() if l.startswith("RESULT")), None)
            print(line if line else "[FAIL %s]\n%s" % (tag, out))
    finally:
        ser.close()


if __name__ == "__main__":
    main()
