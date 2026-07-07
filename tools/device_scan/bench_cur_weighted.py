"""On-device PERFORMANCE benchmark for the weighted-mixed-frames progress
estimate: native cUR (`uUR`) vs the pure-Python ur2 decoder.

The weighted estimate is called once per received frame during a live scan to
update the progress bar; its cost scales with the size of the held mixed-parts
set. This builds an IDENTICAL mid-decode state (a real mixed-parts set, fed from
a mixed-only UR fountain sequence) on both decoders, then times many
`estimated_percent_complete(weight_mixed_frames=True)` calls on each and reports
the per-call microseconds + speedup. Also times the reference
(weight_mixed_frames=False) estimate for context.

    python3 tools/device_scan/bench_cur_weighted.py [--msg-len N] [--reps N]
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))  # tools/ for deploy_app + _devenv
import deploy_app as da  # noqa: E402
import _devenv  # noqa: E402

SS_SRC = _devenv.SS_SRC_ROOT   # $SS_APP_DIR/src (env-driven; see .env.example)
sys.path.insert(0, SS_SRC)
UR2_SRC = _devenv.UR2_SRC

from seedsigner.helpers.ur2.ur import UR  # noqa: E402
from seedsigner.helpers.ur2.ur_encoder import UREncoder  # noqa: E402


def gen_mixed(msg_len, max_frag, want):
    """Deterministic message -> first `want` MIXED fountain parts (seq_num > seq_len)."""
    msg = bytes((i * 37 + 11) & 0xFF for i in range(msg_len))
    ur = UR("bytes", msg)
    seq_len = int(UREncoder(ur, max_fragment_len=max_frag)
                  .next_part().split("/")[1].split("-")[1])
    enc = UREncoder(ur, max_fragment_len=max_frag)
    mixed = []
    n = 0
    while len(mixed) < want and n < want * 30:
        p = enc.next_part()
        n += 1
        if int(p.split("/")[1].split("-")[0]) > seq_len:
            mixed.append(p)
    return mixed, seq_len


DEV = r'''
import sys, uUR, time, gc
if '/' not in sys.path:
    sys.path.insert(0, '/')
from bench_ur2.ur_decoder import URDecoder as Py

parts = [p for p in open('/wparts.txt').read().split('\n') if p]
n = len(parts)
REPS = %(REPS)d

# Pass 1 (native, fast): find the completion point so we can stop the feed just
# short of it, leaving a large mixed-parts set held mid-decode.
d = uUR.URDecoder()
done = n
for i in range(n):
    try: d.receive_part(parts[i])
    except Exception: pass
    if d.is_complete():
        done = i + 1
        break
K = done - 2 if done - 2 > 0 else 1

# Build the IDENTICAL mid-decode state on both decoders.
py = Py()
nat = uUR.URDecoder()
for i in range(K):
    try: py.receive_part(parts[i])
    except Exception: pass
    try: nat.receive_part(parts[i])
    except Exception: pass

mixed_held = len(py.fountain_decoder.mixed_parts)
print('K', K, 'done', done)
print('py_complete', int(py.is_complete()), 'nat_complete', int(nat.is_complete()))
print('mixed_parts_held', mixed_held)
print('py_weighted', '%%.5f' %% py.estimated_percent_complete(weight_mixed_frames=True))
print('nat_weighted', '%%.5f' %% nat.estimated_percent_complete(weight_mixed_frames=True))

def timeit(fn):
    gc.collect()
    t0 = time.ticks_us()
    for _ in range(REPS):
        fn()
    return time.ticks_diff(time.ticks_us(), t0)

t_py_w = timeit(lambda: py.estimated_percent_complete(weight_mixed_frames=True))
t_nat_w = timeit(lambda: nat.estimated_percent_complete(weight_mixed_frames=True))
t_py_r = timeit(lambda: py.estimated_percent_complete(weight_mixed_frames=False))
t_nat_r = timeit(lambda: nat.estimated_percent_complete(weight_mixed_frames=False))

print('REPS', REPS)
print('PY_WEIGHTED_us', t_py_w)
print('NAT_WEIGHTED_us', t_nat_w)
print('PY_REF_us', t_py_r)
print('NAT_REF_us', t_nat_r)
'''


def push_bench_ur2(ser):
    da.raw_exec(ser, "_mkdirp('/bench_ur2')")
    for name in sorted(os.listdir(UR2_SRC)):
        if not name.endswith(".py"):
            continue
        with open(os.path.join(UR2_SRC, name), "rb") as f:
            da.push_bytes(ser, f.read(), "/bench_ur2/%s" % name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--msg-len", type=int, default=6000)
    ap.add_argument("--max-frag", type=int, default=100)
    ap.add_argument("--want", type=int, default=250)
    ap.add_argument("--reps", type=int, default=300)
    ap.add_argument("--no-reset", action="store_true")
    args = ap.parse_args()

    mixed, seq_len = gen_mixed(args.msg_len, args.max_frag, args.want)
    print("[wperf] seq_len=%d, %d mixed-only parts (msg %dB)"
          % (seq_len, len(mixed), args.msg_len))
    blob = ("\n".join(mixed) + "\n").encode()

    ser = da.hard_reset_and_wait(args.port, do_reset=not args.no_reset)
    try:
        da.raw_exec(ser, da.DEVICE_HELPERS)
        print("[wperf] pushing pure-Python ur2 -> /bench_ur2 ...")
        push_bench_ur2(ser)
        print("[wperf] pushing %d parts -> /wparts.txt ..." % len(mixed))
        da.push_bytes(ser, blob, "/wparts.txt")
        print("[wperf] building state + timing on device ...")
        out = da.raw_exec(ser, DEV % {"REPS": args.reps}, timeout=300)
    finally:
        ser.close()

    m = {}
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        print("[dev]", line)
        f = line.split()
        if len(f) == 2:
            m[f[0]] = f[1]

    try:
        reps = int(m["REPS"])
        pw, nw = int(m["PY_WEIGHTED_us"]), int(m["NAT_WEIGHTED_us"])
        pr, nr = int(m["PY_REF_us"]), int(m["NAT_REF_us"])
    except (KeyError, ValueError):
        print("\n[wperf] could not parse timings from device output above")
        return

    print("\n=== weighted estimate: on-device per-call cost ===")
    print("state: %s mixed-parts held (seq_len %d), %s reps/measurement"
          % (m.get("mixed_parts_held", "?"), seq_len, reps))
    print()
    print("                    pure-Python      native cUR      speedup")
    print("weighted estimate  %9.2f us   %9.2f us   %7.1fx"
          % (pw / reps, nw / reps, (pw / nw) if nw else 0))
    print("reference estimate %9.2f us   %9.2f us   %7.1fx"
          % (pr / reps, nr / reps, (pr / nr) if nr else 0))


if __name__ == "__main__":
    main()
