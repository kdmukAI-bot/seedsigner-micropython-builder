#!/usr/bin/env python3
"""Render a KEYED Urdu string on the P4 and capture output, hunting for LVGL
warnings (draw-buf malloc fail, glyph-bitmap fail, asserts) that would explain
the 'vertical bars'. 'اسکین کریں' (Scan) + 'ترتیبات' (Settings) render correctly on
the desktop screenshot generator, so a device-side warning localizes the failure.

    python3 tools/repro_ur_bars.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from sd_format_push import hard_reset_and_wait, raw_exec

SETUP = (
    "import esp\n"
    "esp.osdebug(None)\n"   # silence the verbose sdmmc/cache/intr_alloc flood
    "import json, machine, vfs, os, gc\n"
    "import seedsigner_lvgl as s\n"
    "s.init()\n"
    "try:\n os.listdir('/sd')\nexcept OSError:\n vfs.mount(vfs.VfsFat(machine.SDCard(slot=0,width=4)),'/sd')\n"
    "def L(loc):\n"
    " files=json.loads(s.locale_pack_files(loc)); p={}\n"
    " for f in files: p[f]=open('/sd/'+loc+'/'+f,'rb').read()\n"
    " ok=s.load_locale(loc,p); print('load',loc,ok); return ok\n"
    "import time as _t\n"
    "def R(loc,title,btns):\n"
    " gc.collect(); print('free_before', gc.mem_free())\n"
    " L(loc); s.clear_result_queue()\n"
    " s.button_list_screen({'top_nav':{'title':title,'show_back_button':True,'show_power_button':False},'button_list':btns})\n"
    " for _ in range(80):\n"
    "  if s.poll_for_result() is not None: break\n"
    "  _t.sleep_ms(20)\n"
    " gc.collect(); print('free_after', gc.mem_free()); print('built',loc)\n"
    "print('setup ok')\n"
)


def main():
    ser = hard_reset_and_wait("/dev/ttyACM0")
    print(raw_exec(ser, SETUP, timeout=30), flush=True)

    print("=== en warmup ===", flush=True)
    print(raw_exec(ser, "R('en','Settings',['Single Sig','Multisig','Passphrase'])", timeout=20), flush=True)
    time.sleep(0.3)

    print("=== ur render (KEYED) ===", flush=True)
    print(raw_exec(ser, "R('ur','ترتیبات',['اسکین کریں','ٹھیک ہے','میں سمجھ گیا'])", timeout=25), flush=True)

    print("=== hi render (KEYED, control — should be clean) ===", flush=True)
    print(raw_exec(ser, "R('hi','1 इनपुट',['12 शब्द','12वां शब्द','24 शब्द'])", timeout=25), flush=True)
    print(">>> done", flush=True)


main()
