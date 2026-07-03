"""On-device harness for the Step-4 scan consumer. Deployed as /main.py.

MicroPython auto-runs /main.py on every boot, so **every power-on / USB plug-in
re-initiates the test sequence** with no REPL interaction. After each scan
completes or times out, the harness re-arms and starts a fresh session, so the
board is continuously scanning. Ctrl-C (which the deploy tools send) breaks the
loop to the REPL so new code can be pushed.

Each session: bring up camera_scanner (preview + overlay), build a DecodeQR, run
the poll-loop consumer, and stream a tagged log over the UART so the host can
watch decodes land while a QR is held to the camera.

Prereqs on the device VFS (pushed by tools/run_device_scan.py):
  /lib/scan_consumer.py   this consumer
  /lib/seedsigner/...      the app tree (for DecodeQR)
  /lib/embit/...           decode_qr's import closure
  /lib/secp256k1.py        embit EC fallback shim (native module not in firmware yet)

decode_qr imports pyzbar + urtypes lazily (in-method), so neither is needed to
import it on-device — no pyzbar/urtypes shims. (A UR PSBT scan still needs real
urtypes present to extract the final payload; assembly/completion do not.)
"""
import binascii
import machine
import sys
import time

import camera_scanner

from scan_consumer import run_scan

# Per-session cap so an unattended board re-arms instead of sitting in one scan
# forever (the preview keeps the panel busy the whole time). Ctrl-C also breaks.
SCAN_TIMEOUT_MS = 120_000
# Pause before the re-initiating reset: keeps the result on-screen briefly and
# leaves a window to Ctrl-C into the REPL for a redeploy.
REARM_PAUSE_S = 4


def _log(*args):
    # Single tag so the host stream filter can pick these out of the ESP_LOG noise.
    print("[scan]", *args)


def _on_invalid(payload, why):
    # A NEW payload the decoder rejected (FALSE=4 / INVALID=5) or that raised.
    # Dump length + hex so we can see exactly what k_quirc handed us (a
    # CompactSeedQR must be exactly 16 or 32 bytes to be recognized).
    if isinstance(payload, (bytes, bytearray)):
        _log("ignored NEW payload: status=%r len=%d hex=%s"
             % (why, len(payload), binascii.hexlify(payload[:48]).decode()))
    else:
        _log("ignored NEW payload:", why)


def _summarize(decoder):
    """Best-effort one-line description of what was decoded, for the serial log.
    Defensive: this is a bring-up harness, not the ScanView routing (UR PSBT-byte
    extraction will raise unless real urtypes is present on-device — expected)."""
    try:
        qr_type = decoder.qr_type
    except Exception:
        qr_type = "?"
    _log("COMPLETE  qr_type=%s  pct=%s" % (qr_type, decoder.get_percent_complete()))
    try:
        if decoder.is_seed:
            _log("  seed words:", len(decoder.get_seed_phrase() or []))
        elif decoder.is_psbt:
            _log("  psbt bytes:", len(decoder.get_data_psbt() or b""))
        elif decoder.is_address:
            _log("  address:", decoder.get_address())
        elif decoder.is_settings:
            _log("  settings keys:", list((decoder.get_settings_data() or {}).keys()))
    except Exception as e:
        _log("  (summary unavailable:", e, ")")


def _one_session(DecodeQR):
    """Run a single scan session. Returns True to keep re-arming, False to stop
    (only used for a hard bring-up failure, so we don't spin)."""
    decoder = DecodeQR()
    _log("starting camera_scanner (preview + overlay) ...")
    try:
        camera_scanner.start()
    except Exception as e:
        _log("camera_scanner.start() failed:", e)
        return False  # hardware/bring-up problem — stop, don't hot-loop
    try:
        result = run_scan(
            decoder,
            scanner=camera_scanner,
            timeout_ms=SCAN_TIMEOUT_MS,
            on_progress=lambda pct, status: _log("progress:", pct, "%"),
            on_miss_warning=lambda n: _log(
                "FOUND-BUT-UNREADABLE  consecutive_misses=%d "
                "(move closer / steady up / improve lighting)" % n),
            on_invalid=_on_invalid,
            on_drop=lambda dropped: _log("WARN dropped NEW parts:", dropped),
            log=_log,
        )
    finally:
        # Idempotent teardown: coordinator/overlay/pipeline + render-interval revert.
        camera_scanner.stop()

    _log("result:", result)
    if result.complete:
        _summarize(decoder)
    else:
        _log("ended without completion (%s)" % result.reason)
    return True


def main():
    try:
        from seedsigner.models.decode_qr import DecodeQR
    except ImportError as e:
        _log("cannot import DecodeQR:", e)
        _log("  -> the decode_qr import closure is incomplete on the device VFS.")
        _log("  -> ensure /lib/seedsigner + /lib/embit + /lib/secp256k1.py are pushed")
        _log("     (tools/run_device_scan.py). decode_qr no longer needs pyzbar/urtypes to import.")
        return

    _log("=== scan test; re-initiates on every boot / plug-in ===")
    try:
        ok = _one_session(DecodeQR)
    except KeyboardInterrupt:
        # Deploy tools send Ctrl-C to reclaim the REPL mid-scan.
        _log("interrupted; stopping scanner -> REPL")
        try:
            camera_scanner.stop()
        except Exception:
            pass
        return

    if not ok:
        # Bring-up failed on a fresh boot = a genuine hardware fault; stay in the
        # REPL rather than reset-looping forever.
        _log("camera bring-up failed; staying in REPL")
        return

    # Re-initiate the sequence the way a plug-in does: a full reset re-runs
    # /main.py from a clean camera/ISP init. This deliberately avoids an in-boot
    # stop()->start() re-arm, which fails "pipeline create failed" because the
    # esp_video ISP device (id=20) is not deregistered on cam_pipeline_destroy
    # (C-side teardown leak; the production ScanView start/stop cycle needs it
    # fixed). Ctrl-C during the pause reclaims the REPL for redeploys.
    _log("re-initiating via reset in %ds (Ctrl-C to stay in REPL) ..." % REARM_PAUSE_S)
    try:
        time.sleep(REARM_PAUSE_S)
    except KeyboardInterrupt:
        _log("interrupted; staying in REPL")
        return
    machine.reset()


main()
