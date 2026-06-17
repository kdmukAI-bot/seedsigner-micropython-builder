#!/usr/bin/env python3
"""Reproduce the LVGL-task hang that the interactive tester hit: several en renders
then a ru demo render. On the debug build this should trip heap poisoning / an LVGL
assert / the task WDT and print a panic backtrace on serial.

    python3 tools/repro_ru_freeze.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from sd_format_push import hard_reset_and_wait, raw_exec

SETUP = (
    "import json, machine, vfs, os\n"
    "import seedsigner_lvgl as s\n"
    "s.init()\n"
    "try:\n os.listdir('/sd')\nexcept OSError:\n vfs.mount(vfs.VfsFat(machine.SDCard(slot=0,width=4)),'/sd')\n"
    "def L(loc):\n"
    " files=json.loads(s.locale_pack_files(loc)); p={}\n"
    " for f in files: p[f]=open('/sd/'+loc+'/'+f,'rb').read()\n"
    " return s.load_locale(loc,p)\n"
    "import time as _t\n"
    "def R(loc,title,btns):\n"
    " L(loc); s.clear_result_queue()\n"
    " s.button_list_screen({'top_nav':{'title':title,'show_back_button':True,'show_power_button':False},'button_list':btns})\n"
    " # poll like the tester does — lets the LVGL task FULLY render this screen\n"
    " for _ in range(80):\n"
    "  if s.poll_for_result() is not None: break\n"
    "  _t.sleep_ms(20)\n"
    " print('built',loc)\n"
    "print('setup ok')\n"
)

# en menu -> en demo -> en menu (the 3 renders before the ru tap)
PRE = [
    ("en", "Language", ["en  English", "ru  Russian"]),
    ("en", "Settings", ["Single Sig", "Multisig", "Passphrase"]),
    ("en", "Language", ["en  English", "ru  Russian"]),
]


def main():
    ser = hard_reset_and_wait("/dev/ttyACM0")
    print(raw_exec(ser, SETUP, timeout=30), flush=True)
    for loc, title, btns in PRE:
        print(raw_exec(ser, "R(%r,%r,%r)" % (loc, title, btns), timeout=20), flush=True)
        time.sleep(0.4)

    # The ru demo render — the one that froze. Trigger it, then stream serial so we
    # catch whatever the LVGL render task hits (panic backtrace / assert / WDT).
    print(">>> triggering ru demo render; streaming serial for ~25s ...", flush=True)
    ser.write(b"\r\x03\x03")
    time.sleep(0.1)
    ser.reset_input_buffer()
    ser.write(b"\x01")
    time.sleep(0.3)
    ser.read(8192)
    ser.write("R('ru','Настройки',['Подпись','Кошелёк','Сид-фраза'])\n".encode("utf-8") + b"\x04")
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline:
        c = ser.read(512)
        if c:
            sys.stdout.write(c.decode("utf-8", "replace"))
            sys.stdout.flush()
    ser.close()


if __name__ == "__main__":
    main()
