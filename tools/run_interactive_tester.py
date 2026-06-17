#!/usr/bin/env python3
"""Launch the interactive i18n locale tester on the P4 and stream its output.

Hard-resets first (frees the SDMMC host so mount_sd() can re-create the card),
then pastes tools/esp_i18n_locale_test.py onto the device and echoes serial while
you drive it by TOUCH: tap a language in the menu, tap BACK on a demo screen to
return, tap POWER on the menu to exit.

    python3 tools/run_interactive_tester.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from sd_format_push import hard_reset_and_wait

TESTER = os.path.join(os.path.dirname(__file__), "esp_i18n_locale_test.py")


def main():
    code = open(TESTER, "r").read()
    ser = hard_reset_and_wait("/dev/ttyACM0")   # clears the SDMMC host
    # Paste-mode (Ctrl-E ... Ctrl-D) runs the multi-line tester verbatim.
    ser.write(b"\r\x03\x03")
    time.sleep(0.2)
    ser.reset_input_buffer()
    ser.write(b"\x05")
    time.sleep(0.1)
    ser.read(8192)
    ser.write(code.encode("utf-8") + b"\x04")
    print("=== tester started — TAP a language on the screen ===", flush=True)

    # Completion = the tester's runtime "[i18n] done." — but the pasted SOURCE also
    # contains that literal, so only start watching for it AFTER a runtime-only
    # marker ("ok=True"; the source has "ok=%s") has scrolled past.
    tail = ""
    started = False
    deadline = time.monotonic() + 900
    while time.monotonic() < deadline:
        chunk = ser.read(512)
        if chunk:
            text = chunk.decode("utf-8", "replace")
            sys.stdout.write(text)
            sys.stdout.flush()
            tail = (tail + text)[-300:]
            if not started and "ok=True" in tail:
                started = True
                tail = ""   # drop the echoed source so its 'done' isn't matched
            elif started and "[i18n] done." in tail:
                break
    ser.close()


if __name__ == "__main__":
    main()
