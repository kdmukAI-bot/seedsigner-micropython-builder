"""Replay the exact CPython mixed-part sequence through the URDecoder on-device.

Isolates a MicroPython fountain-solve bug (reduction / Xoshiro256 RNG) from
camera optics: the same mixed parts that complete at part #20 on CPython are fed
straight into the device URDecoder (no camera). Completes ⇒ the animated-scan
stall is optics/coverage; stalls ⇒ a MicroPython solve bug (otherwise invisible,
swallowed by URDecoder.receive_part's `except Exception: return False`).

Prereq: run ur_roundtrip_host.py first to generate ur_parts.json next to this file.
Needs the board connected on /dev/ttyACM0 and quiesced (not in the scan/reset loop).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))  # tools/ for deploy_app
import deploy_app as da

data = json.load(open(os.path.join(HERE, "ur_parts.json")))
parts = data["mixed_parts"]
print("host completed at #%s; feeding %d identical mixed parts to device"
      % (data["host_done_at"], len(parts)))

probe = (
    "from seedsigner.helpers.ur2.ur_decoder import URDecoder\n"
    "parts = " + json.dumps(parts) + "\n"
    "dec = URDecoder()\n"
    "done_at = None\n"
    "for i, p in enumerate(parts):\n"
    "    dec.receive_part(p)\n"
    "    if dec.is_complete():\n"
    "        done_at = i + 1\n"
    "        break\n"
    "print('DEV_is_complete', dec.is_complete(), 'at', done_at,\n"
    "      'processed', dec.fountain_decoder.processed_parts_count)\n"
    "if dec.is_complete():\n"
    "    print('DEV_roundtrip_len', len(dec.result_message().cbor))\n"
    "    print('RESULT: SOLVE_OK (device stall was optics/coverage)')\n"
    "else:\n"
    "    print('RESULT: SOLVE_STALLED (MicroPython fountain-solve bug)')\n"
)

ser = da.hard_reset_and_wait("/dev/ttyACM0", do_reset=False)
try:
    print(da.raw_exec(ser, probe, timeout=60))
finally:
    ser.close()
