#!/usr/bin/env python3
"""Automated i18n gallery: render each locale on the P4 and grab a webcam shot
into device-renders/. Hard-resets once, then per locale loads the pack off the
SD card, renders a demo button-list, and captures /dev/video0.

    python3 tools/esp_i18n_capture.py

Demo strings match the interactive tester (tools/esp_i18n_locale_test.py): the
complex-script titles/buttons are exact runs.bin-keyed corpus text.
"""
import os
import subprocess
import time

from sd_format_push import hard_reset_and_wait, raw_exec

OUT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "device-renders"))

LOCALES = [
    ("en",         "Settings",   ["Single Sig", "Multisig", "Passphrase"]),
    ("ru",         "Настройки",  ["Подпись", "Кошелёк", "Сид-фраза"]),
    ("zh_Hans_CN", "设置",        ["单签", "多签", "设置"]),
    ("fa",         "تنظیمات",    ["امضا", "کیف پول", "عبارت عبور"]),
    ("hi",         "1 इनपुट",    ["12 शब्द", "12वां शब्द", "24 शब्द"]),
    ("th",         "1 อินพุต",   ["12 คำ", "คำที่ 12", "แบบ 12 คำ"]),
    ("ur",         "احتیاط",     ["ٹھیک ہے", "اسکین کریں", "میں سمجھ گیا"]),
]

# Self-contained per-locale snippet: (re)mount SD if needed, stage the pack, load,
# render. %r inlines the locale/title/buttons as MicroPython literals (UTF-8).
SNIPPET = (
    "import json, machine, vfs, os\n"
    "import seedsigner_lvgl as s\n"
    "s.init()\n"
    "try:\n os.listdir('/sd')\nexcept OSError:\n vfs.mount(vfs.VfsFat(machine.SDCard(slot=0, width=4)), '/sd')\n"
    "loc=%r; title=%r; btns=%r\n"
    "files=json.loads(s.locale_pack_files(loc)); packs={}\n"
    "for f in files: packs[f]=open('/sd/'+loc+'/'+f,'rb').read()\n"
    "ok=s.load_locale(loc, packs)\n"
    "s.clear_result_queue()\n"
    "s.button_list_screen({'top_nav':{'title':title,'show_back_button':True,'show_power_button':False},'button_list':btns})\n"
    "print('loaded', loc, 'ok=', ok, 'files=', files)\n"
)


def v4l2(ctrl):
    subprocess.run(["v4l2-ctl", "-d", "/dev/video0", "--set-ctrl", ctrl],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def capture(path):
    subprocess.run(["ffmpeg", "-f", "v4l2", "-video_size", "1280x720",
                    "-i", "/dev/video0", "-frames:v", "1", "-update", "1", "-y", path],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    os.makedirs(OUT, exist_ok=True)
    for c in ("auto_exposure=1", "exposure_time_absolute=300", "gain=140",
              "focus_automatic_continuous=0", "focus_absolute=25", "zoom_absolute=220"):
        v4l2(c)

    ser = hard_reset_and_wait("/dev/ttyACM0")
    try:
        for i, (loc, title, btns) in enumerate(LOCALES):
            print("[cap]", raw_exec(ser, SNIPPET % (loc, title, btns), timeout=30), flush=True)
            time.sleep(1.0)  # let the LVGL task flush the new screen before the shot
            path = os.path.join(OUT, "loc_%02d_%s.jpg" % (i, loc))
            capture(path)
            print("[cap] saved", path, flush=True)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
