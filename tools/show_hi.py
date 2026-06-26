import esp
esp.osdebug(None)
import json, machine, vfs, os
import seedsigner_lvgl as s
s.init()
try:
    os.listdir('/sd')
except OSError:
    vfs.mount(vfs.VfsFat(machine.SDCard(slot=0, width=4)), '/sd')
files = json.loads(s.locale_pack_files('hi'))
p = {}
for f in files:
    p[f] = open('/sd/hi/' + f, 'rb').read()
print('load hi', s.load_locale('hi', p))
s.clear_result_queue()
s.button_list_screen({
    'top_nav': {'title': '1 इनपुट', 'show_back_button': True, 'show_power_button': False},
    'button_list': ['12 शब्द', '12वां शब्द', '24 शब्द'],
})
print('hi shown')
