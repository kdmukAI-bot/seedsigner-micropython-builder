#!/usr/bin/env python3
"""Deploy the SeedSigner Python app + embit onto the P4's internal-flash VFS
over a single held-open serial connection, then optionally run an import-smoke
or boot the Controller.

Unlike the SD card (where a soft-reset clears the mount, forcing one-connection
operation — see sd_format_push.py), the internal-flash FAT VFS persists across
resets, so this is a plain push. We still reuse the held-connection raw-REPL
primitives for speed and a definitive liveness check.

Layout written to the device (no sys.path edits needed — device sys.path already
includes '/lib'):

    /lib/seedsigner/...   <- src/seedsigner/ (minus resources/ unless --resources)
    /lib/embit/...        <- embit/src/embit/

Modes:
    --mode push-only      push source, touch nothing else (default)
    --mode import-smoke    push, then `import seedsigner.controller` on-device and
                           report success / full traceback (no reset, no main.py)
    --mode run            push, write /main.py that calls Controller.start(), reset

By default the app + embit are compiled to `.mpy` bytecode on the host (with the
version-matched mpy-cross from deps/micropython/upstream/mpy-cross) and the .mpy
is pushed instead of raw .py. This kills the 2-4s first-import stall the device
otherwise pays lexing+parsing+compiling big modules (e.g. seed_views.py) from
source. Pass --source to push raw .py instead.

    python3 tools/deploy_app.py [--mode import-smoke] [--clean] [--port /dev/ttyACM0]
"""
import argparse
import atexit
import base64
import os
import subprocess
import sys
import tempfile

# Reuse the proven raw-REPL primitives from the SD pusher.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sd_format_push import hard_reset_and_wait, raw_exec  # noqa: E402

PORT = "/dev/ttyACM0"
SS_SRC = "/home/kdmukai/dev/seedsigner/src/seedsigner"
EMBIT_SRC = "/home/kdmukai/dev/embit/src/embit"
SS_DST = "/lib/seedsigner"
EMBIT_DST = "/lib/embit"

# Host-side mpy-cross. The .mpy bytecode ABI is pinned to the firmware's
# MicroPython version, so we use the mpy-cross built from THIS tree's pinned
# submodule (deps/micropython/upstream) — never a random `pip install mpy-cross`,
# which may emit a mismatched .mpy version. The app is pure Python, so no -march
# is needed (bytecode is architecture-independent).
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
MPY_CROSS_DIR = os.path.join(REPO_ROOT, "deps", "micropython", "upstream", "mpy-cross")
MPY_CROSS_CANDIDATES = (
    os.path.join(MPY_CROSS_DIR, "build", "mpy-cross"),  # current layout
    os.path.join(MPY_CROSS_DIR, "mpy-cross"),           # older layout
)

# The P4 USB-CDC drops bytes on multi-KB transfers (docs/knowledge/
# deploy-serial-truncation.md), which either aborts a batch with a base64 error
# or silently truncates a file. Push is retried this many times; each round
# re-reads the device tree so only missing/mismatched files re-push, and we
# refuse to boot a tree that never verifies clean.
PUSH_ATTEMPTS = 4

# micropython-lib stdlib modules the import closure needs. As of the
# firmware-rebuild milestone, logging + hmac are FROZEN into the board manifest
# (ports/esp32/boards/.../manifest.py), so the harness no longer vendors them to
# /lib. STDLIB_DEPS stays here (empty) as the seam for any future not-yet-frozen
# stdlib dep; each would be a single-module package <name>/<name>.py under the root.
MPY_LIB_STDLIB = ("/home/kdmukai/dev/seedsigner-micropython-builder/deps/micropython/"
                  "upstream/lib/micropython-lib/python-stdlib")
STDLIB_DEPS = []  # logging, hmac now frozen into firmware (was: ["logging", "hmac"])

# NOTE: the former SECP256K1_SHIM (a /lib/secp256k1.py that re-exported embit's
# pure-Python EC fallback) is RETIRED. The firmware now ships a native `secp256k1`
# C module, which resolves before /lib and is ~65x faster for real keys. embit's
# bare `import secp256k1` on MicroPython picks it up directly. See the
# esp-secp256k1 submodule + docs/knowledge/native-secp256k1-static-ecmult-required.md.

# Device-side helpers, defined once in the REPL global namespace and reused
# across subsequent raw_exec calls (globals persist within one connection).
DEVICE_HELPERS = r"""
import os, binascii
def _mkdirp(p):
    cur = ''
    for x in p.strip('/').split('/'):
        cur += '/' + x
        try:
            os.mkdir(cur)
        except OSError:
            pass
def _rmrf(p):
    try:
        st = os.stat(p)
    except OSError:
        return
    if st[0] & 0x4000:
        for e in os.listdir(p):
            _rmrf(p + '/' + e)
        os.rmdir(p)
    else:
        os.remove(p)
def _w(path, b64):
    f = open(path, 'wb'); f.write(binascii.a2b_base64(b64)); f.close()
def _wa(path, b64):
    f = open(path, 'ab'); f.write(binascii.a2b_base64(b64)); f.close()
def _ls(p):
    try:
        st = os.stat(p)
    except OSError:
        return
    if st[0] & 0x4000:
        for e in os.listdir(p):
            _ls(p + '/' + e)
    else:
        print(p, st[6])
print('helpers ready')
"""

IMPORT_SMOKE = r"""
import sys
# Pop the app + its FS-vendored deps so an edited file recompiles. Only seedsigner
# + embit are FS-vendored/editable now; logging + hmac are frozen into firmware and
# secp256k1 is a native C module, so none of those need popping.
for _m in list(sys.modules):
    if _m.split('.')[0] in ('seedsigner', 'embit'):
        del sys.modules[_m]
try:
    import seedsigner.controller
    print('SMOKE_OK')
except Exception as _e:
    sys.print_exception(_e)
    print('SMOKE_FAIL')
"""

MAIN_PY = """\
# Auto-generated by tools/deploy_app.py --mode run. SeedSigner app entry.
import sys
try:
    from seedsigner.controller import Controller
    Controller.get_instance().start()
except Exception as e:
    sys.print_exception(e)
    print('[main] Controller exited via exception; dropping to REPL.')
"""


def _push_big(ser, b64, remote, chunk=10000):
    """A file whose base64 exceeds one batch: stream it in append chunks."""
    off, first = 0, True
    while off < len(b64):
        part = b64[off:off + chunk]
        fn = "_w" if first else "_wa"
        raw_exec(ser, "%s('%s','%s')" % (fn, remote, part), timeout=60)
        off += len(part)
        first = False


def push_bytes(ser, data, remote, batch_bytes=18000):
    b64 = base64.b64encode(data).decode()
    if len(b64) > batch_bytes:
        _push_big(ser, b64, remote)
    else:
        raw_exec(ser, "_w('%s','%s')" % (remote, b64), timeout=60)


def ensure_dev_deps(ser):
    """Vendor any not-yet-frozen stdlib deps (STDLIB_DEPS, currently none) to /lib.
    Cheap and idempotent; keeps import-smoke/run self-contained without a reflash.
    logging + hmac are frozen into firmware; secp256k1 is a native C module."""
    for name in STDLIB_DEPS:
        src = os.path.join(MPY_LIB_STDLIB, name, name + ".py")
        if not os.path.exists(src):
            print("[vendor] WARN micropython-lib module not found:", name)
            continue
        push_bytes(ser, open(src, "rb").read(), "/lib/%s.py" % name)
        print("[vendor] /lib/%s.py" % name)
    # Retire any stale /lib/secp256k1.py dev shim left by an earlier deploy. The
    # native `secp256k1` C module resolves before /lib so the shim is dead code,
    # but remove it to avoid confusion — and so it can't silently mask a missing
    # native module in a firmware built without it.
    raw_exec(ser, "import os\ntry:\n os.remove('/lib/secp256k1.py')\nexcept OSError:\n pass\n",
             timeout=10)


def resolve_mpy_cross(explicit=None, allow_build=True):
    """Return the path to a usable mpy-cross, building it from the pinned
    submodule if it isn't present yet. Order: --mpy-cross, $MPY_CROSS, the
    submodule build dir, then a one-shot `make -C .../mpy-cross`."""
    if explicit:
        if not os.path.exists(explicit):
            sys.exit("[mpy] --mpy-cross not found: %s" % explicit)
        return explicit
    env = os.environ.get("MPY_CROSS")
    if env and os.path.exists(env):
        return env
    for c in MPY_CROSS_CANDIDATES:
        if os.path.exists(c):
            return c
    if not allow_build:
        sys.exit("[mpy] mpy-cross not found; build it: make -C %s" % MPY_CROSS_DIR)
    print("[mpy] mpy-cross not built; building it (one-time) in %s ..." % MPY_CROSS_DIR)
    try:
        subprocess.run(["make", "-C", MPY_CROSS_DIR], check=True)
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        sys.exit("[mpy] failed to build mpy-cross (%s).\n"
                 "      build it manually: make -C %s\n"
                 "      or push raw source with: --source" % (e, MPY_CROSS_DIR))
    for c in MPY_CROSS_CANDIDATES:
        if os.path.exists(c):
            return c
    sys.exit("[mpy] build succeeded but no binary at %s" % (MPY_CROSS_CANDIDATES,))


def mpy_cross_banner(mpy_cross):
    """First line of `mpy-cross --version`, for a logged sanity check that the
    compiler matches the firmware (both come from the same pinned submodule)."""
    try:
        r = subprocess.run([mpy_cross, "--version"], capture_output=True, text=True)
        return ((r.stdout or "") + (r.stderr or "")).strip().splitlines()[0]
    except Exception as e:  # noqa: BLE001 — informational only
        return "version unknown (%s)" % e


# One reusable host temp file for the compiler's -o target (overwritten per call).
_MPY_TMP = []


def _compile_mpy(mpy_cross, src_py, dev_src_name):
    """Compile one .py to .mpy on the host. Return (bytecode_bytes, None) on
    success, or (None, reason) if mpy-cross can't compile it.

    A per-file failure is expected for the rare module using @micropython.native
    /viper, which needs a target -march we deliberately don't set (a wrong-arch
    .mpy would be rejected by the firmware at import). The caller ships those as
    raw .py — they compile on-device only if imported, and none is a module the
    on-device compile stall actually cares about.

    `dev_src_name` is embedded as the source filename (mpy-cross -s) so on-device
    tracebacks read as the import path (e.g. 'seedsigner/views/seed_views.py')
    instead of a host temp path. -s sets the embedded filename only; it does NOT
    strip line numbers, so tracebacks stay fully useful in dev."""
    if not _MPY_TMP:
        fd, path = tempfile.mkstemp(suffix=".mpy", prefix="deploy_")
        os.close(fd)
        _MPY_TMP.append(path)
        atexit.register(lambda: os.path.exists(path) and os.remove(path))
    out = _MPY_TMP[0]
    r = subprocess.run([mpy_cross, "-o", out, "-s", dev_src_name, src_py],
                       capture_output=True, text=True)
    if r.returncode != 0:
        reason = (r.stderr.strip().splitlines() or ["mpy-cross failed"])[-1]
        return None, reason
    with open(out, "rb") as f:
        return f.read(), None


def _dev_src_name(remote_path):
    """Strip the '/lib/' VFS prefix so the embedded traceback filename reads as
    an import path ('seedsigner/...') rather than a device absolute path."""
    return remote_path[5:] if remote_path.startswith("/lib/") else remote_path.lstrip("/")


def _remove_remote(ser, paths, batch_bytes=18000):
    """Batch-delete device files. Used to clear a superseded .py/.mpy counterpart
    left by a prior deploy in the other mode, so the importer can't shadow the
    fresh file with stale bytecode/source."""
    batch, acc = [], 0
    for p in paths:
        call = "_rmrf('%s')" % p
        if acc + len(call) > batch_bytes:
            raw_exec(ser, "\n".join(batch)); batch, acc = [], 0
        batch.append(call); acc += len(call) + 1
    if batch:
        raw_exec(ser, "\n".join(batch))


def push_tree(ser, local_root, remote_root, label, excl_dirs, excl_suffix,
              expected, on_dev=None, force=False, batch_bytes=18000,
              compile_mpy=False, mpy_cross=None):
    """Collect the tree, mkdir -p all dirs in few calls, then write files in
    batched raw_exec payloads (the per-call REPL handshake dominates, so packing
    many small files per call is the win). Records expected sizes into `expected`
    for a single post-pass verification."""
    files, dirs = [], set()
    for dirpath, dirnames, filenames in os.walk(local_root):
        dirnames[:] = [d for d in dirnames if d not in excl_dirs]
        rel = os.path.relpath(dirpath, local_root)
        rdir = remote_root if rel == "." else remote_root + "/" + rel.replace(os.sep, "/")
        dirs.add(rdir)
        for f in filenames:
            if any(f.endswith(s) for s in excl_suffix):
                continue
            files.append((os.path.join(dirpath, f), rdir + "/" + f))

    # Create directories (batch the _mkdirp calls).
    mk, acc = [], 0
    for d in sorted(dirs):
        call = "_mkdirp('%s')" % d
        if acc + len(call) > batch_bytes:
            raw_exec(ser, "\n".join(mk)); mk, acc = [], 0
        mk.append(call); acc += len(call) + 1
    if mk:
        raw_exec(ser, "\n".join(mk))

    state = {"batch": [], "cur": 0, "pushed": 0}
    count = total = skipped = 0

    def flush():
        if state["batch"]:
            raw_exec(ser, "\n".join(state["batch"]), timeout=90)
            state["pushed"] += len(state["batch"])
            state["batch"].clear()
            state["cur"] = 0
            print("[push] %-9s %d files written ..." % (label, state["pushed"]), flush=True)

    stale = []     # device counterparts (.py<->.mpy) a mode switch supersedes
    fallback = []  # (path, reason) for .py shipped as source because mpy-cross balked
    for lp, rp in sorted(files):
        is_py = lp.endswith(".py")
        if compile_mpy and is_py:
            # Ship bytecode: compile on the host so the device skips the
            # lex+parse+compile that stalls first import of big modules.
            data, err = _compile_mpy(mpy_cross, lp, _dev_src_name(rp))
            if data is None:
                fallback.append((rp, err))               # ship raw .py instead
                data = open(lp, "rb").read()
                stale_path = rp[:-3] + ".mpy"             # drop any stale bytecode
            else:
                stale_path = rp                           # old source, if present
                rp = rp[:-3] + ".mpy"
        else:
            data = open(lp, "rb").read()
            stale_path = rp[:-3] + ".mpy" if is_py else None  # old bytecode, if present
        if stale_path and on_dev and stale_path in on_dev:
            stale.append(stale_path)
        expected[rp] = len(data)
        count += 1
        total += len(data)
        if not force and on_dev is not None and on_dev.get(rp) == len(data):
            skipped += 1
            continue  # unchanged on device (size match) — skip for fast iteration
        b64 = base64.b64encode(data).decode()
        if len(b64) > batch_bytes:
            flush()
            _push_big(ser, b64, rp)
            state["pushed"] += 1
            continue
        if state["cur"] + len(b64) > batch_bytes:
            flush()
        state["batch"].append("_w('%s','%s')" % (rp, b64))
        state["cur"] += len(b64)
    flush()
    for rp_, err_ in fallback:
        print("[push] %-9s ship source (mpy-cross: %s): %s" % (label, err_, rp_),
              flush=True)
    if stale:
        _remove_remote(ser, stale, batch_bytes)
        print("[push] %-9s removed %d superseded counterpart(s)" % (label, len(stale)),
              flush=True)
    print("[push] %-9s done: %d files (%d pushed, %d unchanged), %d bytes%s"
          % (label, count, count - skipped, skipped, total,
             " [.mpy]" if compile_mpy else " [.py]"), flush=True)
    return count, total


def device_tree(ser):
    """One recursive os.stat pass; returns {path: size} for both dst roots."""
    out = raw_exec(ser, "_ls('%s'); _ls('%s'); print('LS_END')" % (SS_DST, EMBIT_DST),
                   timeout=30)
    on_dev = {}
    for line in out.splitlines():
        line = line.strip()
        if not line or line == "LS_END":
            continue
        try:
            path, sz = line.rsplit(" ", 1)
            on_dev[path] = int(sz)
        except ValueError:
            pass
    return on_dev


def verify(ser, expected):
    on_dev = device_tree(ser)
    missing = [p for p in expected if p not in on_dev]
    mismatch = [(p, expected[p], on_dev[p]) for p in expected
                if p in on_dev and on_dev[p] != expected[p]]
    extra = [p for p in on_dev if p not in expected]
    print("[verify] device has %d files; expected %d; missing %d; size-mismatch %d; extra %d"
          % (len(on_dev), len(expected), len(missing), len(mismatch), len(extra)))
    for p in missing[:20]:
        print("  MISSING ", p)
    for p, e, g in mismatch[:20]:
        print("  MISMATCH", p, "expected", e, "got", g)
    return not missing and not mismatch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=PORT)
    ap.add_argument("--mode", default="push-only",
                    choices=["push-only", "import-smoke", "run", "smoke-only"])
    ap.add_argument("--clean", action="store_true",
                    help="recursively remove /lib/seedsigner and /lib/embit first")
    ap.add_argument("--force", action="store_true",
                    help="re-push every file even if its size matches the device")
    ap.add_argument("--resources", action="store_true",
                    help="also push src/seedsigner/resources (25MB; skipped by default)")
    ap.add_argument("--source", action="store_true",
                    help="push raw .py instead of host-compiled .mpy (default is .mpy)")
    ap.add_argument("--mpy-cross", default=None,
                    help="path to mpy-cross (default: build from the pinned submodule)")
    ap.add_argument("--no-build-mpy-cross", action="store_true",
                    help="fail instead of auto-building mpy-cross if it's missing")
    ap.add_argument("--seedsigner-src", default=SS_SRC,
                    help="seedsigner package dir to push (default: %(default)s). Point at a "
                         "git-archive export to pin the push to a committed tip instead of a "
                         "live (possibly drifting) working tree.")
    ap.add_argument("--embit-src", default=EMBIT_SRC,
                    help="embit package dir to push (default: %(default)s)")
    args = ap.parse_args()
    print("[deploy] seedsigner src: %s" % args.seedsigner_src)
    print("[deploy] embit src:      %s" % args.embit_src)

    compile_mpy = not args.source
    mpy_cross = None
    if compile_mpy and args.mode != "smoke-only":  # smoke-only pushes nothing
        mpy_cross = resolve_mpy_cross(args.mpy_cross,
                                      allow_build=not args.no_build_mpy_cross)
        print("[mpy] using %s (%s)" % (mpy_cross, mpy_cross_banner(mpy_cross)))

    print("[deploy] poll-opening REPL (no hard reset) ...")
    ser = hard_reset_and_wait(args.port, do_reset=False)
    try:
        raw_exec(ser, DEVICE_HELPERS)

        # smoke-only: don't push the app tree, just (re-)vendor deps + import test.
        if args.mode == "smoke-only":
            print("[deploy] smoke-only: vendor deps + import seedsigner.controller ...")
            ensure_dev_deps(ser)
            out = raw_exec(ser, IMPORT_SMOKE, timeout=60)
            print("---- device output ----\n%s\n-----------------------" % out)
            print("[deploy] RESULT:", "PASS" if "SMOKE_OK" in out else "FAIL")
            return

        if args.clean:
            print("[deploy] cleaning %s and %s ..." % (SS_DST, EMBIT_DST))
            raw_exec(ser, "_rmrf('%s'); _rmrf('%s'); print('cleaned')" % (SS_DST, EMBIT_DST))

        ss_excl = {"__pycache__"} if args.resources else {"__pycache__", "resources"}

        # Push with auto-retry against racy CDC truncation. Each round re-reads
        # the device tree (device_tree), so push_tree's size-match skip re-pushes
        # only the files that are missing or truncated; a dropped-chunk batch that
        # raises is retried too. We proceed only once verify() is clean.
        ok = False
        expected = {}
        for attempt in range(1, PUSH_ATTEMPTS + 1):
            expected = {}
            on_dev = {} if (args.clean and attempt == 1) else device_tree(ser)
            force = args.force and attempt == 1  # after round 1, only re-push bad files
            try:
                push_tree(ser, args.seedsigner_src, SS_DST, "seedsigner", ss_excl, (".pyc",),
                          expected, on_dev, force,
                          compile_mpy=compile_mpy, mpy_cross=mpy_cross)
                push_tree(ser, args.embit_src, EMBIT_DST, "embit", {"__pycache__"}, (".pyc",),
                          expected, on_dev, force,
                          compile_mpy=compile_mpy, mpy_cross=mpy_cross)
                ok = verify(ser, expected)
            except RuntimeError as e:
                print("[deploy] push error (attempt %d/%d): %s" % (attempt, PUSH_ATTEMPTS, e))
                ok = False
            if ok:
                print("[deploy] verify: PASS (attempt %d/%d)" % (attempt, PUSH_ATTEMPTS))
                break
            if attempt < PUSH_ATTEMPTS:
                print("[deploy] verify FAILED — re-pushing missing/mismatched files ...")
        if not ok:
            sys.exit("[deploy] verify: FAIL after %d attempts — refusing to boot a "
                     "partial/truncated tree. Check the USB-CDC link and re-run." % PUSH_ATTEMPTS)
        ensure_dev_deps(ser)

        if args.mode == "import-smoke":
            print("[deploy] running import smoke: import seedsigner.controller ...")
            out = raw_exec(ser, IMPORT_SMOKE, timeout=60)
            print("---- device output ----")
            print(out)
            print("-----------------------")
            print("[deploy] RESULT:", "PASS" if "SMOKE_OK" in out else "FAIL")
        elif args.mode == "run":
            print("[deploy] writing /main.py and resetting to boot Controller ...")
            b64 = base64.b64encode(MAIN_PY.encode()).decode()
            raw_exec(ser, "_f=open('/main.py','wb'); _f.write(binascii.a2b_base64('%s')); "
                          "_f.close(); print('main.py written')" % b64)
    finally:
        ser.close()
    print("[deploy] done.")


if __name__ == "__main__":
    main()
