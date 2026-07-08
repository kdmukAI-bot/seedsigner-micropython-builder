#!/usr/bin/env python3
"""Hard-reset the P4, format its SD card (FAT) on-device, and push the language
packs over a single held-open serial connection.

Why this exists: opening the USB-serial port soft-resets the board, which clears
MicroPython's VFS mount but NOT the ESP-IDF SDMMC host (an init'd host then
refuses re-init with EBUSY). So SD setup cannot span separate REPL invocations —
it must run on ONE connection, after ONE hard reset that actually frees the host.

    python3 tools/sd_format_push.py [--packs DIR] [--port /dev/ttyACM0]

The pack SOURCE points ONLY at the app (this repo copies the app's bundled bytes, it does
NOT know the pack repo or build packs): `--packs DIR` overrides; otherwise
`_devenv.resolve_packs()` = `$SS_APP_DIR/src/lang-packs` (or `SS_PACKS_DIR` — see
.env.example). Empty/absent = a valid English-only deploy (clean card, no packs staged).
The full self-contained pack is staged to /sd/<locale>/... — every runtime file at its
subpath: the subset .ttf(s) + runs.bin + endonym_<h>.bin + manifest.json +
LC_MESSAGES/messages.mo (the app reads .mo from that subpath and the picker fetches the
endonym images). Debug artifacts (runs.json) are skipped.
"""
import argparse
import base64
import os
import sys
import time

import serial  # pyserial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _devenv  # env-driven local-dev paths (no hard-coded /home/... in committed files)

PORT = "/dev/ttyACM0"


def _is_runtime_file(rel):
    """True for a pack file the DEVICE loads, keyed by its path relative to the pack
    root ("<locale>/..."). Stages the subset font(s), pre-shaped runs, endonym images,
    the self-describing manifest, and the compiled catalog at its LC_MESSAGES subpath;
    skips debug artifacts (runs.json)."""
    base = rel.rsplit("/", 1)[-1]
    if rel.endswith("/LC_MESSAGES/messages.mo"):
        return True
    if base in ("manifest.json", "runs.bin"):
        return True
    if base.endswith(".ttf"):
        return True
    return base.startswith("endonym_") and base.endswith(".bin")


def _read_until(ser, token, deadline):
    buf = b""
    while time.monotonic() < deadline:
        b = ser.read(512)
        if b:
            buf += b
            if token in buf:
                return buf
    return buf


def raw_exec(ser, code, timeout=30):
    """Run code via the raw REPL on an already-open connection (no reset)."""
    ser.write(b"\r\x03\x03")
    time.sleep(0.05)
    ser.reset_input_buffer()
    ser.write(b"\x01")
    _read_until(ser, b"raw REPL", time.monotonic() + 3)
    ser.write(code.encode() + b"\x04")
    out = _read_until(ser, b"\x04>", time.monotonic() + timeout)
    ser.write(b"\x02")
    text = out.decode("utf-8", "replace")
    if text.startswith("OK"):
        text = text[2:]
    parts = text.split("\x04")
    stdout = parts[0].strip()
    stderr = parts[1].strip() if len(parts) > 1 else ""
    if stderr:
        raise RuntimeError("device error:\n" + stderr)
    return stdout


def hard_reset_and_wait(port, do_reset=True):
    """Hard-reset via machine.reset() (frees the stuck SDMMC host), then poll-open
    until the REPL answers a real command. A definitive raw-exec liveness check is
    more reliable than banner string-matching across the USB-CDC re-enumeration."""
    if do_reset:
        try:
            s = serial.Serial(port, 115200, timeout=0.2)
            s.write(b"\r\x03\x03import machine; machine.reset()\r")
            time.sleep(0.5)
            s.close()
        except Exception:
            pass
        time.sleep(4.0)  # reboot + USB-CDC re-enumeration (debug build boots slower)
    deadline = time.monotonic() + 50
    while time.monotonic() < deadline:
        try:
            # write_timeout bounds ser.write(): the P4 USB-CDC can stall its RX
            # buffer mid-push (byte-drop / re-enumeration), and without a write
            # timeout pyserial blocks forever at 0% CPU. A timeout turns that into
            # a raised SerialTimeoutException the caller can retry on a fresh fd.
            s = serial.Serial(port, 115200, timeout=0.3, write_timeout=20)
        except Exception:
            time.sleep(0.5)
            continue
        try:
            if "READY" in raw_exec(s, "print('REA' + 'DY')", timeout=4):
                return s
        except Exception:
            pass
        try:
            s.close()
        except Exception:
            pass
        time.sleep(0.4)
    raise SystemExit("device did not return to REPL after reset")


def collect_pack_files(packs_dir):
    """Every runtime file across all locale packs under `packs_dir`, as
    (host_path, relpath) where relpath is "<locale>/..." (LC_MESSAGES/ preserved).
    Debug artifacts (runs.json) are skipped by _is_runtime_file."""
    rels = []
    if os.path.isdir(packs_dir):
        for loc in sorted(os.listdir(packs_dir)):
            loc_dir = os.path.join(packs_dir, loc)
            if loc.startswith(".") or not os.path.isdir(loc_dir):
                continue
            for root, _dirs, fnames in os.walk(loc_dir):
                for fn in fnames:
                    full = os.path.join(root, fn)
                    rel = os.path.relpath(full, packs_dir).replace(os.sep, "/")
                    if _is_runtime_file(rel):
                        rels.append((full, rel))
    rels.sort(key=lambda fr: fr[1])
    return rels


def _push_file(ser, remote, data, chunk=12000):
    """Write `data` to `remote` on-device, chunking the base64 (in multiples of 4 chars,
    so each fragment decodes standalone) — a large font sent as one USB-CDC write
    truncates, surfacing on-device as an a2b_base64 'incorrect padding' error. Relies on
    the on-device `_mkdirp` (defined once in main) to create the file's subdir. Returns
    the device-reported size (str)."""
    b64 = base64.b64encode(data).decode()  # chunk % 4 == 0 keeps every fragment valid base64
    raw_exec(ser, "_mkdirp('%s')" % remote.rsplit("/", 1)[0])
    for i in range(0, len(b64), chunk):
        raw_exec(ser,
            "import binascii\n"
            "_f = open('%s', '%s'); _f.write(binascii.a2b_base64('%s')); _f.close()\n"
            % (remote, "wb" if i == 0 else "ab", b64[i:i + chunk]),
            timeout=90)
    return raw_exec(ser, "import os; print(os.stat('%s')[6])" % remote)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--packs", default=None,
                    help="dir of built pack bytes (<locale>/...); default = the app's "
                         "$SS_APP_DIR/src/lang-packs via _devenv.resolve_packs()")
    ap.add_argument("--port", default=PORT)
    args = ap.parse_args()

    packs = args.packs or _devenv.resolve_packs()

    # Enumerate the source BEFORE touching the card. Empty/absent is NOT an error: the app
    # bundles no packs -> a valid English-only deploy (we format a clean card, stage nothing;
    # the app renders its baked English floor). This repo only COPIES the app's bundled bytes.
    rels = collect_pack_files(packs)
    if rels:
        n_locales = len({rel.split("/", 1)[0] for _f, rel in rels})
        print("[sd] staging %d files across %d locales from %s" % (len(rels), n_locales, packs))
    else:
        print("[sd] no packs at %s -> English-only deploy: formatting a clean card, staging "
              "nothing.\n"
              "     (Point --packs / SS_PACKS_DIR at a built packs dir, or build packs into the\n"
              "     app's src/lang-packs, to deploy non-English locales.)" % packs)

    print("[sd] hard-resetting board to free the SDMMC host ...")
    ser = hard_reset_and_wait(args.port)
    print("[sd] REPL alive; formatting SD (FAT) ...")
    try:
        out = raw_exec(ser,
            "import vfs, machine, os\n"
            "sd = machine.SDCard(slot=0, width=4)\n"   # slot 0 = IOMUX; VDD via LDO_VO4 (firmware)
            "vfs.VfsFat.mkfs(sd)\n"
            "vfs.mount(vfs.VfsFat(sd), '/sd')\n"
            "print('formatted', os.listdir('/sd'))\n", timeout=30)
        print("[sd]", out)

        # mkdir -p helper on-device, defined once (globals persist across raw_exec on
        # this held-open connection); packs carry a LC_MESSAGES/ subdir to create.
        raw_exec(ser,
            "import os\n"
            "def _mkdirp(p):\n"
            " c=''\n"
            " for x in p.strip('/').split('/'):\n"
            "  c+='/'+x\n"
            "  try:\n"
            "   os.mkdir(c)\n"
            "  except OSError:\n"
            "   pass\n")

        for full, rel in rels:
            data = open(full, "rb").read()
            sz = _push_file(ser, "/sd/" + rel, data)
            status = "OK" if sz == str(len(data)) else "MISMATCH(dev=%s)" % sz
            print("[sd] %-30s %8d bytes  %s" % (rel, len(data), status), flush=True)

        print("[sd] --- final card contents ---")
        print(raw_exec(ser,
            "import os\n"
            "def _walk(p):\n"
            " for e in sorted(os.listdir(p)):\n"
            "  f = p + '/' + e\n"
            "  try:\n"
            "   os.listdir(f); _walk(f)\n"       # a dir -> recurse
            "  except OSError:\n"
            "   print(f, os.stat(f)[6])\n"       # a file -> path + size
            "_walk('/sd')\n", timeout=30))
    finally:
        ser.close()
    print("[sd] done.")


if __name__ == "__main__":
    main()
