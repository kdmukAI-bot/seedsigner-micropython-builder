#!/usr/bin/env python3
"""Per-screen render cost, en vs ur, after the locale is already loaded. Each
button_list_screen() build must take the LVGL lock the previous frame's flush
holds, so render[i] absorbs the flush of render[i-1] — i.e. the steady-state value
reflects build + flush (the user-perceived "tap -> visible" cost)."""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from sd_format_push import hard_reset_and_wait, raw_exec

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
    "def SERIES(loc,title,btns,n=6):\n"
    " ld=L(loc); xs=[RR(title,btns) for _ in range(n)]\n"
    " return 'load=%dms renders=%s' % (ld, xs)\n"
    "print('setup ok')\n"
)


def main():
    ser = hard_reset_and_wait("/dev/ttyACM0")
    print(raw_exec(ser, SETUP, timeout=30).strip())
    for loc, title, btns in [
        ("en", "Settings", ["Single Sig", "Multisig", "Passphrase"]),
        ("hi", "1 इनपुट", ["12 शब्द", "12वां शब्द", "24 शब्द"]),
        ("ur", "احتیاط", ["ٹھیک ہے", "اسکین کریں", "میں سمجھ گیا"]),
    ]:
        out = raw_exec(ser, "print(SERIES(%r,%r,%r))" % (loc, title, btns), timeout=45)
        print("%-4s %s" % (loc, out.strip().splitlines()[-1] if out.strip() else "(no output)"))


main()
