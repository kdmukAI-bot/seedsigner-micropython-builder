#!/usr/bin/env python3
"""Deploy + run the Step-4 QR-scan consumer on the P4, streaming the serial log.

Builder-side dev harness. Pushes the closure DecodeQR needs (seedsigner + embit +
the secp256k1 shim, reused from deploy_app), then the consumer (scan_consumer.py)
and /main.py (main_scan.py). Since MicroPython auto-runs /main.py, the scan test
re-initiates on every boot / USB plug-in; this driver reopens the port (which
resets the board) and streams so you can watch decodes land while holding a QR to
the camera.

No reflash needed — the camera_scanner C module is already in the flashed
firmware; this only writes to the persistent VFS.

decode_qr.py imports pyzbar + urtypes LAZILY (in-method) as of lvgl/mpy-renderer
b9b3748, so the module imports on-device without them — no pyzbar/urtypes shims
needed. (secp256k1 is still shimmed via deploy_app: embit's native EC module is
not built into the firmware yet.)

Usage:
    python3 tools/run_device_scan.py [--port /dev/ttyACM0] [--skip-app]
                                     [--stream-seconds 120] [--no-stream]
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import serial  # pyserial
import deploy_app as da  # reuse the proven push / verify / REPL primitives

DEVICE_SCAN = os.path.join(HERE, "device_scan")
PORT = "/dev/ttyACM0"


def _push_file(ser, local, remote):
    da.push_bytes(ser, open(local, "rb").read(), remote)
    print("[push]", remote, flush=True)


def deploy(ser, skip_app):
    da.raw_exec(ser, da.DEVICE_HELPERS)

    if skip_app:
        print("[deploy] --skip-app: assuming seedsigner+embit already on /lib")
    else:
        on_dev = da.device_tree(ser)
        expected = {}
        da.push_tree(ser, da.SS_SRC, da.SS_DST, "seedsigner",
                     {"__pycache__", "resources"}, (".pyc",), expected, on_dev, False)
        da.push_tree(ser, da.EMBIT_SRC, da.EMBIT_DST, "embit",
                     {"__pycache__"}, (".pyc",), expected, on_dev, False)
        da.verify(ser, expected)

    da.ensure_dev_deps(ser)  # /lib/secp256k1.py shim (+ any not-yet-frozen stdlib)

    # The consumer (the /main.py entry is written last, after the smoke below).
    _push_file(ser, os.path.join(DEVICE_SCAN, "scan_consumer.py"), "/lib/scan_consumer.py")

    # Import-smoke over the held raw REPL: /main.py is not written yet, so nothing
    # is auto-running — this is a clean check that the decode_qr closure resolves
    # on-device (the risky part). raw_exec raises with the device traceback on any
    # import error, so a failure surfaces loudly here instead of on the next boot.
    print("[smoke] import scan_consumer + DecodeQR() ...", flush=True)
    out = da.raw_exec(
        ser,
        "import sys\n"
        "for _m in list(sys.modules):\n"
        "    if _m.split('.')[0] in ('seedsigner','embit','secp256k1','scan_consumer'):\n"
        "        del sys.modules[_m]\n"
        "import scan_consumer\n"
        "from seedsigner.models.decode_qr import DecodeQR\n"
        "DecodeQR()\n"
        "print('IMPORT_' + 'OK')\n",
        timeout=60,
    )
    print("[smoke]", "PASS" if "IMPORT_OK" in out else "FAIL", flush=True)

    # The auto-run entry point (written last so the smoke above ran clean).
    _push_file(ser, os.path.join(DEVICE_SCAN, "main_scan.py"), "/main.py")


def stream(port, seconds):
    print("\n[stream] reopening %s (this resets the board; /main.py boots) ..." % port)
    print("[stream] >>> HOLD A QR (SeedQR / animated UR / BBQr) TO THE CAMERA <<<")
    print("[stream] (Ctrl-C to stop streaming)\n", flush=True)
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.dtr = False
    ser.rts = False
    ser.timeout = 0
    ser.open()
    end = time.time() + seconds
    try:
        while time.time() < end:
            n = ser.in_waiting
            if n:
                sys.stdout.write(ser.read(n).decode("utf-8", "replace"))
                sys.stdout.flush()
            else:
                time.sleep(0.02)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
    print("\n[stream] done.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=PORT)
    ap.add_argument("--skip-app", action="store_true",
                    help="skip pushing seedsigner+embit (already on /lib)")
    ap.add_argument("--stream-seconds", type=int, default=120)
    ap.add_argument("--no-stream", action="store_true",
                    help="deploy only; do not reset+stream")
    ap.add_argument("--stream-only", action="store_true",
                    help="skip deploy; just reset+stream the already-deployed /main.py")
    args = ap.parse_args()

    if not args.stream_only:
        print("[deploy] opening REPL on %s (no hard reset) ..." % args.port)
        ser = da.hard_reset_and_wait(args.port, do_reset=False)
        try:
            deploy(ser, args.skip_app)
        finally:
            ser.close()
        print("[deploy] done.")

    if not args.no_stream:
        stream(args.port, args.stream_seconds)


if __name__ == "__main__":
    main()
