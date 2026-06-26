# Worst-case font-cache saturation test  (docs/font-memory-plan.md, Task E)
# Sweeps every distinct glyph of the densest locale (zh_Hans_CN, 468 glyphs) through
# the 20px button, 23px title and 18px body caches to saturate the per-(font,px)
# cache-index trees (cap 256 each). WARNING: this is designed to push the 128KB LVGL
# pool to its limit. If it overflows, the board freezes (~10s WDT) then reboots --
# that reboot IS a result (proves the overflow risk). Watch used% / big as it climbs.
GLYPHS = "#%()+,-./0123456789:;=?@ABCDEFGHILMNOPQRSTUVX[]^_abcdefghijklmnoprstuvwxyz{|}\u00b0\u201c\u201d\u2026\u3001\u3002\u4e00\u4e0a\u4e0b\u4e0d\u4e0e\u4e25\u4e2a\u4e2d\u4e34\u4e3a\u4e3b\u4e45\u4e49\u4e4b\u4e86\u4e8b\u4e8c\u4e8e\u4e9b\u4ea4\u4eae\u4eba\u4ec5\u4ecb\u4ecd\u4ece\u4ee5\u4eec\u4ef6\u4efd\u4f19\u4f1a\u4f20\u4f34\u4f4d\u4f4e\u4f5c\u4f7f\u4fdd\u4fe1\u4fee\u503c\u50cf\u5165\u5168\u516c\u516d\u5173\u5177\u5185\u5199\u51c6\u51d1\u51fa\u51fb\u5207\u521b\u522b\u5230\u5236\u524d\u529e\u529f\u52a0\u52a8\u52a9\u52ff\u5305\u5339\u533a\u5341\u5355\u5361\u5373\u539f\u53ca\u53cd\u53d1\u53d6\u53d7\u53ef\u5408\u540c\u540d\u5417\u5426\u542b\u542f\u544a\u5458\u548c\u5546\u5668\u56de\u56fe\u5728\u5730\u5740\u578b\u589e\u5904\u5907\u591a\u5931\u5934\u5957\u59cb\u5b50\u5b58\u5b89\u5b8c\u5b9a\u5b9e\u5ba1\u5bc6\u5bf9\u5bfc\u5c06\u5c11\u5c1a\u5c55\u5d4c\u5de5\u5df2\u5e01\u5e76\u5e9f\u5ea6\u5efa\u5f00\u5f03\u5f0f\u5f15\u5f52\u5f53\u5f84\u5f85\u5fc5\u6001\u6027\u606f\u60a8\u60c5\u610f\u6210\u6211\u6216\u6240\u624b\u624d\u6253\u6269\u626b\u627e\u6284\u62b9\u62cd\u62d4\u62e9\u6301\u6307\u6309\u6350\u636e\u6388\u63a5\u63b7\u63cf\u63d0\u63d2\u6444\u64cd\u652f\u6536\u6539\u653e\u653f\u6548\u6570\u65ad\u65b0\u65b9\u65bd\u65cb\u65e0\u65e7\u65f6\u6613\u662f\u663e\u666f\u66f4\u6700\u6709\u671f\u672a\u672c\u673a\u6743\u6765\u677e\u6790\u67d0\u67e5\u6807\u6821\u6838\u683c\u68c0\u6b21\u6b3e\u6b63\u6b64\u6b65\u6bd4\u6cc4\u6cd5\u6ce8\u6d3e\u6d4b\u6d4f\u6d88\u6df7\u6dfb\u6e05\u6e90\u70b9\u7167\u71b5\u7248\u7279\u72b6\u72ec\u73b0\u751f\u7528\u7531\u7535\u754c\u7559\u7591\u7684\u76ee\u76f4\u76f8\u770b\u77e5\u77ed\u77ff\u7801\u786c\u786e\u793a\u793e\u7981\u79bb\u79c1\u79d2\u79fb\u7acb\u7b26\u7b2c\u7b56\u7b7e\u7b97\u7ba1\u7c7b\u7cfb\u7d22\u7d27\u7ea7\u7eb9\u7ebf\u7ec8\u7ecf\u7edc\u7edf\u7ee7\u7eed\u7ef4\u7f16\u7f51\u7f6e\u7f72\u7ffb\u8054\u80cc\u80fd\u811a\u81ea\u81f3\u8272\u82b1\u83dc\u884c\u8868\u88ab\u8981\u89c1\u89c8\u89e3\u8a00\u8b66\u8ba1\u8ba4\u8bb0\u8bbe\u8bbf\u8bc1\u8bc6\u8bcd\u8bd5\u8be5\u8be6\u8bed\u8bef\u8bf7\u8bfb\u8d25\u8d26\u8d39\u8d44\u8d4f\u8d60\u8def\u8df3\u8f6c\u8f7d\u8f91\u8f93\u8fc7\u8fd4\u8fd9\u8fdb\u8fdc\u8fde\u8ff0\u9000\u9001\u9009\u901a\u9053\u90e8\u90fd\u914d\u91c7\u91cd\u91d1\u94a5\u94ae\u94b1\u94fe\u9519\u957f\u95ea\u95ee\u95f4\u9605\u964d\u9664\u968f\u9690\u9694\u96f6\u9700\u9732\u9759\u9762\u9875\u9879\u987b\u9884\u989c\u989d\u9a8c\u9ab0\u9ad8\u9ed8\uff01\uff08\uff09\uff0c\uff1a\uff1b\uff1f"

import json, machine, vfs, os, time as t
import seedsigner_lvgl_screens as ss
ss.init(); ss.set_screensaver_timeout(0)
try: os.listdir('/sd')
except OSError: vfs.mount(vfs.VfsFat(machine.SDCard(slot=0, width=4)), '/sd')
loc='zh_Hans_CN'
p={f: open('/sd/'+loc+'/'+f,'rb').read() for f in json.loads(ss.locale_pack_files(loc))}
print('load', loc, ss.load_locale(loc, p))

def stat(tag):
    m=ss.mem_stats()
    print('%-14s used=%2d%% max=%-6d frag=%2d%% big=%-7d int_free=%d' % (
        tag, m['lvgl_used_pct'], m['lvgl_max_used'], m['lvgl_frag_pct'],
        m['lvgl_free_biggest'], m['internal_free']))

def chunks(s, n):
    return [s[i:i+n] for i in range(0, len(s), n)]

stat('baseline')
G=GLYPHS
BTN=8; NB=7; STEP=BTN*NB

# Sweeps 1-2: 20px buttons + 23px title (twice, to saturate past LRU eviction)
for sweep in (1, 2):
    for i in range(0, len(G), STEP):
        seg=G[i:i+STEP]
        btns=[b for b in chunks(seg, BTN) if b] or [' ']
        ss.clear_result_queue()
        ss.button_list_screen({'top_nav': {'title': G[i:i+10] or ' ',
            'show_back_button': True, 'show_power_button': False}, 'button_list': btns})
        t.sleep_ms(60)
        stat('s%d btn %d' % (sweep, i))

# Sweep 3: 18px body (large_icon_status text), small chunks so all glyphs are visible
for i in range(0, len(G), 40):
    seg=G[i:i+40]
    ss.clear_result_queue()
    ss.large_icon_status_screen({'top_nav': {'title': G[i:i+10] or ' ',
        'show_back_button': False, 'show_power_button': False},
        'status_type': 'warning', 'status_headline': G[i:i+12] or ' ', 'text': seg,
        'button_list': [G[i:i+6] or 'OK'], 'is_bottom_list': True, 'warning_edges': False})
    t.sleep_ms(60)
    stat('body %d' % i)

stat('FINAL')
print('--- done ---')
