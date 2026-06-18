#!/usr/bin/env python3
"""Hard-reset the P4, format its SD card (FAT) on-device, and push the language
packs over a single held-open serial connection.

Why this exists: opening the USB-serial port soft-resets the board, which clears
MicroPython's VFS mount but NOT the ESP-IDF SDMMC host (an init'd host then
refuses re-init with EBUSY). So SD setup cannot span separate REPL invocations —
it must run on ONE connection, after ONE hard reset that actually frees the host.

    python3 tools/sd_format_push.py [--packs DIR] [--port /dev/ttyACM0]

The pack layout written to the card mirrors the source: /sd/<locale>/<file>,
where <file> is the .ttf(s) + runs.bin that ss_locale_pack_files() asks for.
"""
import argparse
import base64
import glob
import os
import time

import serial  # pyserial

PORT = "/dev/ttyACM0"
DEFAULT_PACKS = "/home/kdmukai/dev/seedsigner-lvgl-screens/lang-packs"
LOADABLE = ("*.ttf", "runs.bin")


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
            s = serial.Serial(port, 115200, timeout=0.3)
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--packs", default=DEFAULT_PACKS)
    ap.add_argument("--port", default=PORT)
    args = ap.parse_args()

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

        locales = sorted(d for d in os.listdir(args.packs)
                         if os.path.isdir(os.path.join(args.packs, d)))
        for loc in locales:
            raw_exec(ser, "import os\ntry:\n os.mkdir('/sd/%s')\nexcept OSError:\n pass" % loc)
            files = []
            for pat in LOADABLE:
                files += glob.glob(os.path.join(args.packs, loc, pat))
            for path in sorted(files):
                name = os.path.basename(path)
                data = open(path, "rb").read()
                b64 = base64.b64encode(data).decode()
                # One REPL round-trip per file: the bytes fly over USB-CDC fast; it's
                # the per-call raw-REPL handshake that's slow, so minimize calls.
                sz = raw_exec(ser,
                    "import binascii, os\n"
                    "_d = binascii.a2b_base64('%s')\n"
                    "_f = open('/sd/%s/%s', 'wb'); _f.write(_d); _f.close()\n"
                    "print(os.stat('/sd/%s/%s')[6])\n" % (b64, loc, name, loc, name),
                    timeout=90)
                status = "OK" if sz == str(len(data)) else "MISMATCH(dev=%s)" % sz
                print("[sd] %-11s %-18s %7d bytes  %s" % (loc, name, len(data), status), flush=True)

        print("[sd] --- final card contents ---")
        print(raw_exec(ser,
            "import os\n"
            "for d in sorted(os.listdir('/sd')):\n"
            " for f in sorted(os.listdir('/sd/'+d)):\n"
            "  print('/sd/'+d+'/'+f, os.stat('/sd/'+d+'/'+f)[6])\n", timeout=20))
    finally:
        ser.close()
    print("[sd] done.")


if __name__ == "__main__":
    main()
