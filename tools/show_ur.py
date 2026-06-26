import esp
esp.osdebug(None)
import json, machine, vfs, os
import seedsigner_lvgl as s
s.init()
try:
    os.listdir('/sd')
except OSError:
    vfs.mount(vfs.VfsFat(machine.SDCard(slot=0, width=4)), '/sd')
files = json.loads(s.locale_pack_files('ur'))
p = {}
for f in files:
    p[f] = open('/sd/ur/' + f, 'rb').read()
print('load ur', s.load_locale('ur', p))
s.clear_result_queue()
s.button_list_screen({
    'top_nav': {'title': 'ترتیبات', 'show_back_button': True, 'show_power_button': False},
    'button_list': ['اسکین کریں', 'ٹھیک ہے', 'میں سمجھ گیا'],
})
print('ur shown (held on screen for webcam)')
