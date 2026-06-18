#!/usr/bin/env python3
"""Stress the tiny_ttf glyph cache the way the interactive flow does: render the
FULL 7-locale menu (lots of glyphs) interleaved with locale demo screens, many
times. On firmware where glyph bitmaps land in the 64 KB LVGL pool this OOM-spins
(WDT panic). With glyph bitmaps routed to PSRAM it should complete cleanly.

    python3 tools/stress_locale_churn.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from sd_format_push import hard_reset_and_wait, raw_exec

SETUP = (
    "import json, machine, vfs, os, gc\n"
    "import seedsigner_lvgl as s\n"
    "s.init()\n"
    "try:\n os.listdir('/sd')\nexcept OSError:\n vfs.mount(vfs.VfsFat(machine.SDCard(slot=0,width=4)),'/sd')\n"
    "MENU=['en  English','ru  Russian (Cyrillic)','zh  Chinese (CJK)','fa  Farsi (RTL)',"
    "'hi  Hindi (Devanagari)','th  Thai','ur  Urdu (Nastaliq, RTL)']\n"
    "DEMO={'en':('Settings',['Single Sig','Multisig','Passphrase']),"
    "'ru':('Настройки',['Подпись','Кошелёк','Сид-фраза']),"
    "'zh_Hans_CN':('设置',['单签','多签','设置'])}\n"
    "def L(loc):\n"
    " files=json.loads(s.locale_pack_files(loc)); p={}\n"
    " for f in files: p[f]=open('/sd/'+loc+'/'+f,'rb').read()\n"
    " return s.load_locale(loc,p)\n"
    "def menu():\n"
    " L('en'); s.clear_result_queue()\n"
    " s.button_list_screen({'top_nav':{'title':'Language','show_back_button':False,'show_power_button':True},'button_list':MENU})\n"
    " _settle()\n"
    "def demo(loc):\n"
    " L(loc); t,b=DEMO[loc]; s.clear_result_queue()\n"
    " s.button_list_screen({'top_nav':{'title':t,'show_back_button':True,'show_power_button':False},'button_list':b})\n"
    " _settle()\n"
    "def _settle():\n"
    " for _ in range(40):\n"
    "  if s.poll_for_result() is not None: break\n"
    "  time.sleep_ms(20)\n"
    "import time\n"
    "print('setup ok, free=', gc.mem_free())\n"
)


def main():
    ser = hard_reset_and_wait("/dev/ttyACM0")
    print(raw_exec(ser, SETUP, timeout=30), flush=True)
    # Many rounds: full menu (7 labels) then a demo, cycling locales. This is far
    # more glyph churn than the gallery's single render of each.
    seq = ["en", "ru", "zh_Hans_CN", "ru", "en", "zh_Hans_CN", "ru", "en", "ru", "zh_Hans_CN"]
    for i, loc in enumerate(seq):
        try:
            print("[%d] menu ..." % i, raw_exec(ser, "menu()", timeout=25), flush=True)
            print("[%d] demo %s ..." % (i, loc), raw_exec(ser, "demo(%r)" % loc, timeout=25), flush=True)
        except Exception as e:
            print("[%d] FAILED at %s: %r" % (i, loc, e), flush=True)
            # device likely panicked/wedged — stream serial for the backtrace
            t0 = time.time()
            while time.time() - t0 < 15:
                c = ser.read(512)
                if c:
                    sys.stdout.write(c.decode("utf-8", "replace")); sys.stdout.flush()
            break
    else:
        print(">>> STRESS COMPLETE — no freeze. mem_free now:",
              raw_exec(ser, "import gc; print(gc.mem_free())", timeout=10), flush=True)
    ser.close()


if __name__ == "__main__":
    main()
