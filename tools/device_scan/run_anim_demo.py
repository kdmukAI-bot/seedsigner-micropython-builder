"""Deploy anim_demo.py as /main.py and stream, so the overlay progress-glide can
be watched on the panel WITHOUT an animated QR (drives the bar via report()).

    python3 tools/device_scan/run_anim_demo.py [--stream-seconds 60]

Restore the normal scan test afterwards with:
    python3 tools/run_device_scan.py --skip-app --no-stream
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

import serial
import deploy_app as da


def stream(port, seconds):
    print("\n[stream] reopening %s (resets board; anim_demo boots) ..." % port)
    print("[stream] >>> WATCH THE PROGRESS BAR: snaps to 8%, then GLIDES "
          "8->20->35->58->80->100 <<<")
    print("[stream] (Ctrl-C to stop)\n", flush=True)
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
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--stream-seconds", type=int, default=60)
    args = ap.parse_args()

    print("[anim] pushing anim_demo.py -> /main.py ...")
    ser = da.hard_reset_and_wait(args.port, do_reset=False)
    try:
        da.raw_exec(ser, da.DEVICE_HELPERS)
        da.push_bytes(ser, open(os.path.join(HERE, "anim_demo.py"), "rb").read(), "/main.py")
        print("[anim] pushed.")
    finally:
        ser.close()
    stream(args.port, args.stream_seconds)


if __name__ == "__main__":
    main()
