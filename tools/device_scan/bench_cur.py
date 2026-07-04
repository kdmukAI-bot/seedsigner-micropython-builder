"""On-device A/B benchmark: native cUR (`uUR`) vs pure-Python `ur2` URDecoder.

Feeds an identical animated BC-UR PSBT part list to BOTH decoders on the P4,
times each `receive_part()` per frame with `time.ticks_us`, asserts the two
reassembled CBOR payloads are byte-identical, and prints a per-frame table plus
totals + speedup. This is the headline test from the cUR integration plan
(builder docs/cur-ur-decoder-integration-plan.md): the per-frame native-vs-pure
differential is the number that matters.

Default fixture is the btc-datagen 100-input 2-of-3 P2WSH multisig UR PSBT
(98 animated parts) — a deliberate stress case well past the original report.

Prereq: firmware built with `uUR` baked in, flashed to the board on
/dev/ttyACM0. Reuses tools/deploy_app.py's serial/push primitives.

    python3 tools/device_scan/bench_cur.py [--fixture PATH] [--port DEV] [--csv OUT]
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))  # tools/ for deploy_app
import deploy_app as da  # noqa: E402

DEFAULT_FIXTURE = "/home/kdmukai/dev/btc-datagen/output/psbt_2of3_p2wsh_100in_normal_parts.txt"
UR2_SRC = "/home/kdmukai/dev/seedsigner/src/seedsigner/helpers/ur2"

# Device-side benchmark. Runs in the raw REPL; DEVICE_HELPERS globals persist in
# the same connection but this block is self-contained apart from /bench_parts.txt
# and (optionally) the pushed /bench_ur2 package.
DEV_BENCH = r'''
import sys, time, gc
import uUR
try:
    from seedsigner.helpers.ur2.ur_decoder import URDecoder as PyURDecoder
    PYSRC = 'app-tree'
except Exception:
    if '/' not in sys.path:
        sys.path.insert(0, '/')
    from bench_ur2.ur_decoder import URDecoder as PyURDecoder
    PYSRC = 'bench_ur2'

parts = [p for p in open('/bench_parts.txt').read().split('\n') if p]
n = len(parts)

def bench(recv, done):
    ts = []
    at = None
    for i in range(n):
        t0 = time.ticks_us()
        try:
            recv(parts[i])
        except Exception:
            pass
        ts.append(time.ticks_diff(time.ticks_us(), t0))
        if at is None and done():
            at = i + 1
    return ts, at

gc.collect()
py = PyURDecoder()
t_py, d_py = bench(py.receive_part, py.is_complete)
cbor_py = bytes(py.result_message().cbor) if py.is_complete() else b''
del py
gc.collect()

nat = uUR.URDecoder()
t_nat, d_nat = bench(nat.receive_part, nat.is_complete)
cbor_nat = bytes(nat.result.cbor) if nat.is_complete() else b''
del nat
gc.collect()

print('PYSRC', PYSRC)
print('N', n)
print('DONE_PY', d_py, 'DONE_NAT', d_nat)
print('MATCH', cbor_py == cbor_nat, 'LEN_PY', len(cbor_py), 'LEN_NAT', len(cbor_nat))
print('TABLE_START')
for i in range(n):
    print(i + 1, t_py[i], t_nat[i])
print('TABLE_END')
'''


def app_tree_ur2_present(ser):
    out = da.raw_exec(
        ser,
        "try:\n"
        " from seedsigner.helpers.ur2.ur_decoder import URDecoder\n"
        " print('UR2_APP_OK')\n"
        "except Exception as e:\n"
        " print('UR2_APP_NO', e)\n",
        timeout=20,
    )
    return "UR2_APP_OK" in out


def push_bench_ur2(ser):
    da.raw_exec(ser, "_mkdirp('/bench_ur2')")
    for name in sorted(os.listdir(UR2_SRC)):
        if not name.endswith(".py"):
            continue
        with open(os.path.join(UR2_SRC, name), "rb") as f:
            da.push_bytes(ser, f.read(), "/bench_ur2/%s" % name)
        print("[push] /bench_ur2/%s" % name)


def summarize(pairs):
    """pairs = list of (idx, py_us, nat_us). Print summary + speedup."""
    py = [p[1] for p in pairs]
    nat = [p[2] for p in pairs]
    tot_py, tot_nat = sum(py), sum(nat)

    def stats(xs):
        xs = sorted(xs)
        return xs[0], xs[len(xs) // 2], xs[-1], sum(xs) / len(xs)

    p_min, p_med, p_max, p_mean = stats(py)
    n_min, n_med, n_max, n_mean = stats(nat)
    print()
    print("=== per-frame (microseconds) ===")
    print("            min      median      max        mean       total")
    print("pure-py  %8d  %8d  %9d  %9.1f  %10d" % (p_min, p_med, p_max, p_mean, tot_py))
    print("native   %8d  %8d  %9d  %9.1f  %10d" % (n_min, n_med, n_max, n_mean, tot_nat))
    print()
    if tot_nat:
        print("TOTAL speedup (pure-py / native): %.1fx" % (tot_py / tot_nat))
    if n_mean:
        print("PER-FRAME mean speedup:           %.1fx" % (p_mean / n_mean))
    print("wall-clock per pass: pure-py %.1f ms  |  native %.1f ms"
          % (tot_py / 1000.0, tot_nat / 1000.0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--csv", default=os.path.join(HERE, "bench_cur_frames.csv"))
    ap.add_argument("--no-reset", action="store_true",
                    help="assume the board is already at a quiescent REPL")
    args = ap.parse_args()

    parts_bytes = open(args.fixture, "rb").read()
    n_parts = len([l for l in parts_bytes.split(b"\n") if l])
    print("[bench] fixture=%s (%d parts, %d bytes)"
          % (os.path.basename(args.fixture), n_parts, len(parts_bytes)))

    ser = da.hard_reset_and_wait(args.port, do_reset=not args.no_reset)
    try:
        da.raw_exec(ser, da.DEVICE_HELPERS)
        print("[bench] uUR present:",
              "uUR" in da.raw_exec(ser, "import uUR; print('uUR')", timeout=10))

        if app_tree_ur2_present(ser):
            print("[bench] pure-Python decoder: deployed app tree")
        else:
            print("[bench] pure-Python decoder: pushing standalone /bench_ur2 ...")
            push_bench_ur2(ser)

        print("[bench] pushing fixture -> /bench_parts.txt ...")
        da.push_bytes(ser, parts_bytes, "/bench_parts.txt")

        print("[bench] running A/B benchmark on device ...")
        out = da.raw_exec(ser, DEV_BENCH, timeout=240)
    finally:
        ser.close()

    # Parse device output.
    pairs = []
    in_table = False
    meta = {}
    for line in out.splitlines():
        line = line.strip()
        if line == "TABLE_START":
            in_table = True
            continue
        if line == "TABLE_END":
            in_table = False
            continue
        if in_table:
            f = line.split()
            if len(f) == 3:
                pairs.append((int(f[0]), int(f[1]), int(f[2])))
        elif line:
            print("[dev]", line)
            k = line.split()
            if k:
                meta[k[0]] = k[1:]

    if not pairs:
        print("\n[bench] no per-frame data parsed; raw device output:\n", out)
        return

    with open(args.csv, "w") as f:
        f.write("frame,pure_py_us,native_us\n")
        for idx, p, nn in pairs:
            f.write("%d,%d,%d\n" % (idx, p, nn))
    print("\n[bench] per-frame CSV -> %s" % args.csv)

    summarize(pairs)


if __name__ == "__main__":
    main()
