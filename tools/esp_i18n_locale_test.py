"""Interactive i18n locale tester for the ESP32-P4 (MIPI-DSI + GT911 touch).

Reads language packs from the SD card (/sd/<locale>/<file>), loads each via the
shared LVGL i18n loader, and renders a localized demo screen. Drive it by TOUCH:
tap a language in the menu; on a demo screen tap the top-nav BACK arrow to return;
on the menu tap the POWER icon to exit. Mirrors the Pi Zero tester
(seedsigner-raspi-lvgl/tests/pi_i18n_locale_test.py).

Run from the host (board freshly booted), letting it run while you tap:
    python3 tools/mpy_repl.py run tools/esp_i18n_locale_test.py --soft-reset --timeout 600
or copy to the device and `import esp_i18n_locale_test`.

Exercises every pack TYPE on real hardware: the baked Western floor (en), the
Cyrillic / CJK / Farsi packs (ru/zh/fa), and the Devanagari / Thai / Nastaliq
complex-script glyph-run packs (hi/th/ur).
"""
import time
import json

import machine
import vfs
import os

import seedsigner_lvgl as s

SD = "/sd"

# (locale_id, ASCII menu label, demo_title, [demo buttons]). The demo strings are
# real corpus text confirmed to render on-device; the complex-script ones (hi/th/
# ur) are EXACT runs.bin-keyed strings — arbitrary text in those scripts has no
# pre-shaped glyph run and would render as tofu.
LOCALES = [
    ("en",         "en  English",              "Settings",   ["Single Sig", "Multisig", "Passphrase"]),
    ("ru",         "ru  Russian (Cyrillic)",   "Настройки",  ["Подпись", "Кошелёк", "Сид-фраза"]),
    ("zh_Hans_CN", "zh  Chinese (CJK)",        "设置",        ["单签", "多签", "设置"]),
    ("fa",         "fa  Farsi (RTL)",          "تنظیمات",    ["امضا", "کیف پول", "عبارت عبور"]),
    ("hi",         "hi  Hindi (Devanagari)",   "1 इनपुट",    ["12 शब्द", "12वां शब्द", "24 शब्द"]),
    ("th",         "th  Thai",                 "1 อินพุต",   ["12 คำ", "คำที่ 12", "แบบ 12 คำ"]),
    ("ur",         "ur  Urdu (Nastaliq, RTL)", "احتیاط",     ["ٹھیک ہے", "اسکین کریں", "میں سمجھ گیا"]),
]

# On ESP32 the back/power top-nav taps arrive as a button_selected result whose
# index is the reserved code (see SEEDSIGNER_RET_* in seedsigner.h).
RET_BACK = 1000
RET_POWER = 1001


def mount_sd():
    try:
        os.listdir(SD)          # already mounted (e.g. re-run)?
        return
    except OSError:
        pass
    # slot 0 = IOMUX (pins automatic); VDD is powered by LDO_VO4 in the firmware.
    sd = machine.SDCard(slot=0, width=4)
    vfs.mount(vfs.VfsFat(sd), SD)


def load_locale(loc):
    """Stage the locale's pack files off the SD card and hand the bytes to the loader."""
    files = json.loads(s.locale_pack_files(loc))   # e.g. ["th.ttf", "runs.bin"]
    packs = {}
    for f in files:
        with open("%s/%s/%s" % (SD, loc, f), "rb") as fh:
            packs[f] = fh.read()
    ok = s.load_locale(loc, packs)
    print("[i18n] load_locale(%r) files=%s ok=%s" % (loc, files, ok))
    return ok


def poll_until_result():
    while True:
        ev = s.poll_for_result()
        if ev is not None:
            return ev
        time.sleep_ms(20)


def show_list(title, buttons, *, back, power):
    s.clear_result_queue()
    s.button_list_screen({
        "top_nav": {"title": title, "show_back_button": back, "show_power_button": power},
        "button_list": buttons,
    })
    return poll_until_result()


def main():
    s.init()
    mount_sd()
    print("[i18n] ready: tap a language; BACK on a demo screen; POWER on the menu to exit.")
    while True:
        load_locale("en")   # the language menu is always English so it stays legible
        kind, idx, label = show_list("Language", [r[1] for r in LOCALES], back=False, power=True)
        if kind == "button_selected" and idx == RET_POWER:
            break
        if kind != "button_selected" or idx >= len(LOCALES):
            continue
        loc, _, title, buttons = LOCALES[idx]
        load_locale(loc)
        while True:
            k2, i2, l2 = show_list(title, buttons, back=True, power=False)
            if k2 == "button_selected" and i2 == RET_BACK:
                break
            print("[i18n]   selected:", (k2, i2, l2))
    print("[i18n] done.")


main()
