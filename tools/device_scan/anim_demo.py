"""Overlay progress-glide demo (deploy as /main.py).

Drives the preview overlay's progress bar through a scripted percent sequence via
camera_scanner.report(), so the lv_anim glide can be watched on the panel WITHOUT
needing an animated QR to decode. Exercises exactly the animation path a multi-part
scan would hit: the first report snaps (0 -> first value), each later report glides.
Re-inits via reset each cycle; Ctrl-C during the pause reclaims the REPL.

Self-contained: only needs the firmware camera_scanner module (no seedsigner/DecodeQR).
"""
import machine
import time

import camera_scanner as cs

# First value snaps (bar appearing); the rest glide. Uneven gaps mimic a real
# multi-part scan's chunky per-fragment advances.
SEQ = [8, 20, 35, 35, 58, 80, 100]
STEP_MS = 900  # dwell between reports; > the 300ms glide so each glide is visible


def _log(*a):
    print("[anim]", *a)


def demo():
    _log("start camera_scanner (preview + overlay) ...")
    cs.start()
    try:
        for pct in SEQ:
            cs.report(cs.FRAME_NEW, pct)   # dedup drops the repeated 35 -> no-op
            _log("report", pct, "%")
            time.sleep_ms(STEP_MS)
        cs.report_complete()
        _log("complete; holding so the final glide settles")
        time.sleep_ms(1500)
    finally:
        cs.stop()


_log("=== overlay glide demo; re-inits each cycle, Ctrl-C for REPL ===")
try:
    demo()
except KeyboardInterrupt:
    try:
        cs.stop()
    except Exception:
        pass
    _log("interrupted -> REPL")
    raise SystemExit

_log("re-initiating via reset in 4s (Ctrl-C to stay in REPL) ...")
try:
    time.sleep(4)
except KeyboardInterrupt:
    raise SystemExit
machine.reset()
