#!/usr/bin/env python3
"""Sweep every i18n locale on the P4: load it, render a demo screen twice (cold +
warm), report timings, and grab a webcam frame per locale. Confirms all locales
render and quantifies the first-click (cold) vs subsequent (warm) cost.

    python3 tools/confirm_locales.py
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
from sd_format_push import hard_reset_and_wait, raw_exec

CAPDIR = os.path.join(os.path.dirname(__file__), "..", ".tmp")

# (locale, title, [buttons]) — exact runs.bin-keyed strings, mirrors esp_i18n_locale_test.
LOCALES = [
    ("en",         "Settings",   ["Single Sig", "Multisig", "Passphrase"]),
    ("ru",         "Настройки",  ["Подпись", "Кошелёк", "Сид-фраза"]),
    ("zh_Hans_CN", "设置",        ["单签", "多签", "设置"]),
    ("fa",         "تنظیمات",    ["امضا", "کیف پول", "عبارت عبور"]),
    ("hi",         "1 इनपुट",    ["12 शब्द", "12वां शब्द", "24 शब्द"]),
    ("th",         "1 อินพุต",   ["12 คำ", "คำที่ 12", "แบบ 12 คำ"]),
    ("ur",         "احتیاط",     ["ٹھیک ہے", "اسکین کریں", "میں سمجھ گیا"]),
]

SETUP = (
    "import esp\n"
    "esp.osdebug(None)\n"
    "import json, machine, vfs, os, time as t\n"
    "import seedsigner_lvgl as s\n"
    "s.init()\n"
    "try:\n os.listdir('/sd')\nexcept OSError:\n vfs.mount(vfs.VfsFat(machine.SDCard(slot=0,width=4)),'/sd')\n"
    "def L(loc):\n"
    " f=json.loads(s.locale_pack_files(loc)); p={}\n"
    " for x in f: p[x]=open('/sd/'+loc+'/'+x,'rb').read()\n"
    " a=t.ticks_ms(); s.load_locale(loc,p); return t.ticks_diff(t.ticks_ms(),a)\n"
    "def RR(title,btns):\n"
    " s.clear_result_queue(); a=t.ticks_ms()\n"
    " s.button_list_screen({'top_nav':{'title':title,'show_back_button':True,'show_power_button':False},'button_list':btns})\n"
    " return t.ticks_diff(t.ticks_ms(),a)\n"
    "def M(loc,title,btns):\n"
    " ld=L(loc); r1=RR(title,btns); r2=RR(title,btns); t.sleep_ms(300)\n"
    " return 'load=%dms cold=%dms warm=%dms' % (ld,r1,r2)\n"
    "print('setup ok')\n"
)


def main():
    os.makedirs(CAPDIR, exist_ok=True)
    ser = hard_reset_and_wait("/dev/ttyACM0")
    print(raw_exec(ser, SETUP, timeout=30).strip())
    print("=" * 60)
    for loc, title, btns in LOCALES:
        out = raw_exec(ser, "print(M(%r,%r,%r))" % (loc, title, btns), timeout=45)
        timing = out.strip().splitlines()[-1] if out.strip() else "(no output)"
        fn = os.path.join(CAPDIR, "sweep_%s.jpg" % loc)
        subprocess.run(["ffmpeg", "-loglevel", "error", "-f", "v4l2", "-i", "/dev/video0",
                        "-frames:v", "1", "-update", "1", "-y", fn], check=False)
        print("%-12s %-40s -> %s" % (loc, timing, fn))
    print("=" * 60)
    print("done")


main()
