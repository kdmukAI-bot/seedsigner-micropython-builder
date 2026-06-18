#!/usr/bin/env python3
"""Minimal MicroPython raw-REPL driver over a serial port.

Drives the board's REPL without mpremote (not in the build image). Uses pyserial,
which is already available on the host and in the Docker base image.

Usage:
    python3 tools/mpy_repl.py exec "import seedsigner_lvgl; print(dir(seedsigner_lvgl))"
    python3 tools/mpy_repl.py run path/to/script.py
    python3 tools/mpy_repl.py stream "code that prints continuously" --seconds 60

`exec`/`run` use the raw REPL (Ctrl-A, code, Ctrl-D) and print the program's
output once it finishes. `stream` pastes the code at the normal REPL and then
echoes serial output for N seconds — for interactive on-device testers that loop
and print as the user touches the screen.
"""
import argparse
import sys
import time

import serial  # pyserial

PORT = "/dev/ttyACM0"
BAUD = 115200


def _open(port):
    return serial.Serial(port, BAUD, timeout=0.1)


def _read_until(ser, token, deadline):
    buf = b""
    while time.monotonic() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            if token in buf:
                return buf
    return buf  # timed out; return what we have


def soft_reset(ser):
    # Ctrl-D at the friendly REPL soft-resets the runtime: clears globals AND runs
    # peripheral deinit hooks (frees a stuck SDMMC host, etc.). Wait for the banner.
    ser.write(b"\r\x03\x03")
    time.sleep(0.1)
    ser.reset_input_buffer()
    ser.write(b"\x04")
    _read_until(ser, b">>>", time.monotonic() + 5)
    time.sleep(0.2)


def exec_code(ser, code, timeout, do_soft_reset=False):
    if do_soft_reset:
        soft_reset(ser)
    # Interrupt anything running, enter raw REPL.
    ser.write(b"\r\x03\x03")
    time.sleep(0.1)
    ser.reset_input_buffer()
    ser.write(b"\x01")  # Ctrl-A -> raw REPL
    _read_until(ser, b"raw REPL", time.monotonic() + 3)
    # Send code + Ctrl-D to execute.
    ser.write(code.encode("utf-8") + b"\x04")
    deadline = time.monotonic() + timeout
    out = _read_until(ser, b"\x04>", deadline)  # output ... \x04 [err] \x04 >
    ser.write(b"\x02")  # Ctrl-B -> back to friendly REPL
    # Strip the raw-REPL framing: device sends "OK" + stdout + \x04 + stderr + \x04 + ">".
    text = out.decode("utf-8", "replace")
    if text.startswith("OK"):
        text = text[2:]
    return text.replace("\x04", "").rstrip(">").rstrip()


def stream_code(ser, code, seconds):
    ser.write(b"\r\x03\x03")
    time.sleep(0.2)
    ser.reset_input_buffer()
    # Paste at the friendly REPL via paste mode (Ctrl-E ... Ctrl-D) so multi-line
    # code with blank lines runs as written.
    ser.write(b"\x05")  # Ctrl-E -> paste mode
    time.sleep(0.1)
    ser.read(256)
    ser.write(code.encode("utf-8") + b"\x04")  # Ctrl-D ends paste -> executes
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        chunk = ser.read(256)
        if chunk:
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
    # Leave the program running on the device; interrupt with Ctrl-C next time.


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["exec", "run", "stream"])
    ap.add_argument("arg", help="code string, or .py path for `run`")
    ap.add_argument("--port", default=PORT)
    ap.add_argument("--timeout", type=float, default=30.0, help="exec/run: seconds to wait for completion")
    ap.add_argument("--seconds", type=float, default=60.0, help="stream: seconds to echo output")
    ap.add_argument("--soft-reset", action="store_true", help="soft-reset the runtime before exec/run")
    args = ap.parse_args()

    code = args.arg
    if args.mode == "run":
        with open(args.arg) as f:
            code = f.read()

    ser = _open(args.port)
    try:
        if args.mode == "stream":
            stream_code(ser, code, args.seconds)
        else:
            sys.stdout.write(exec_code(ser, code, args.timeout, do_soft_reset=args.soft_reset))
            sys.stdout.write("\n")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
