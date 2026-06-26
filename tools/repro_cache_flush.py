# Cache-flush recovery test v2 (docs/font-memory-plan.md Task E) -- the deciding test
# between a flush-at-safe-points strategy and Approach A.
# 1) Fragment HARD with ~15 varied CJK screens.  2) unload_locale() to retire the CJK
# fonts.  3) Render an ASCII-only screen: this deletes the last CJK screen -> reaps the
# CJK caches (frees their nodes) while adding only tiny Latin-floor allocations, so `big`
# now reflects the FREED-cache state, not a fresh CJK render.
#   big RECOVERS toward baseline -> the cache holes DO coalesce; a flush strategy works.
#   big STAYS collapsed          -> the holes are scattered/un-coalescable -> Approach A.
# HARD-RESET the board before running.
VARIED = ["...", "0\u00b0", "1 \u4e2a\u8f93\u5165", "12 \u4e2a\u5355\u8bcd", "180\u00b0", "24 \u4e2a\u5355\u8bcd", "270\u00b0", "5 \u79d2", "90\u00b0", "BIP-39 \u5355\u8bcd\u8868\u7d22\u5f15", "BIP-39 \u5bc6\u7801\u77ed\u8bed", "BIP-85 \u5b50\u52a9\u8bb0\u8bcd", "BIP-85 \u6570\u8bcd", "BIP-85 \u7d22\u5f15", "BIP-85 \u7d22\u5f15\u51fa\u9519", "BTC", "Electrum \u52a9\u8bb0\u8bcd", "Electrum \u8b66\u544a", "I/O \u6d4b\u8bd5", "OP_RETURN", "SD \u5361\u5df2\u63d2\u5165", "SD \u5361\u5df2\u79fb\u9664", "SeedQR \u683c\u5f0f", "Taproot", "Xpub", "Xpub \u4e8c\u7ef4\u7801\u683c\u5f0f", "[ ... ]", "btc", "sats", "tBtc", "tSats", "\u4e0b\u4e00\u4e2a\u6536\u6b3e\u65b9", "\u4e0b\u4e00\u6b65", "\u4e0d\u652f\u6301\u7684\u811a\u672c\u7c7b\u578b\uff01", "\u4e34\u65f6\u5185\u5b58\u4e2d\u7684\u8bbe\u7f6e\u5df2\u66f4\u65b0", "\u4e34\u754c\u503c\u4e3a 0.01", "\u4e3b\u7f51", "\u4e3b\u9875", "\u4e8c\u7ef4\u7801\u5bc6\u5ea6", "\u4e8c\u7ef4\u7801\u80cc\u666f\u989c\u8272", "\u4ea4\u6613\u51fa\u9519", "\u4ea4\u6613\u8ba1\u7b97", "\u4ec5\u652f\u6301\u539f\u751f\u9694\u79bb\u89c1\u8bc1", "\u4ee5 0 \u4e3a\u6700\u7ec8\u786e\u5b9a\u503c", "\u4f20\u7edf", "\u4fdd\u7559\u52a9\u8bb0\u8bcd", "\u4fee\u6539\u6307\u7eb9", "\u4fee\u6539\u8bbe\u7f6e", "\u5168\u989d\u652f\u51fa\uff01", "\u5173\u673a", "\u5185\u5b58\u4e2d\u7684\u52a9\u8bb0\u8bcd", "\u51fa\u9519", "\u51fa\u9519\uff01", "\u521b\u5efa\u4e00\u4e2a\u52a9\u8bb0\u8bcd", "\u52a0\u8f7d\u4e00\u4e2a\u52a9\u8bb0\u8bcd", "\u52a0\u8f7d\u52a9\u8bb0\u8bcd", "\u52a0\u8f7d\u52a9\u8bb0\u8bcd\u4e0e\u4e4b\u7b7e\u540d", "\u52a0\u8f7d\u52a9\u8bb0\u8bcd\u4ee5\u8fdb\u884c\u9a8c\u8bc1", "\u52a8\u6001\uff08\u9ed8\u8ba4\uff09", "\u52a9\u8bb0\u8bcd", "\u52a9\u8bb0\u8bcd\u8bed\u8a00", "\u52a9\u8bb0\u8bcd\u957f\u5ea6", "\u5355\u4f4d\u663e\u793a", "\u5355\u51fb\u6309\u94ae", "\u5355\u7b7e\u540d", "\u5355\u8bcd\u4e0d\u5bf9\uff01", "\u5355\u8bcd\u9009\u62e9\u7684\u71b5", "\u539f\u59cb\u5341\u516d\u8fdb\u5236\u6570\u636e", "\u539f\u59cb\u71b5\u4f4d", "\u539f\u751f\u9694\u79bb\u89c1\u8bc1", "\u53cd\u8f6c\u989c\u8272", "\u53cd\u9762 = 0", "\u53d6\u6d88", "\u53ef\u7591\u4ea4\u6613", "\u53ef\u968f\u65f6\u65ad\u7535\uff0c\u5df2\u786e\u4fdd\u5b89\u5168", "\u56de\u5f52\u6d4b\u8bd5\u7f51", "\u5730\u5740\u5df2\u9a8c\u8bc1\uff01", "\u5730\u5740\u6d4f\u89c8\u5668", "\u5730\u5740\u9a8c\u8bc1\u5df2\u5931\u8d25", "\u5730\u5740\u9a8c\u8bc1\u6210\u529f", "\u589e\u52a0\u4eae\u5ea6", "\u5907\u4efd\u52a9\u8bb0\u8bcd", "\u5907\u4efd\u5df2\u7ecf\u8fc7\u9a8c\u8bc1", "\u591a\u91cd\u7b7e\u540d", "\u591a\u91cd\u7b7e\u540d\u9a8c\u8bc1", "\u5b50\u7d22\u5f15\u65e0\u6548", "\u5b8c\u6210", "\u5ba1\u6838 SeedQR", "\u5ba1\u6838\u4e0e\u7f16\u8f91", "\u5ba1\u6838\u4ea4\u6613", "\u5ba1\u6838\u6536\u6b3e\u65b9", "\u5ba1\u6838\u8be6\u60c5", "\u5bfc\u51fa xpub", "\u5bfc\u51fa\u6269\u5c55\u516c\u94a5", "\u5c06\u53d1\u9001", "\u5c06\u8bbe\u7f6e\u4fdd\u5b58\u5230 SD \u5361", "\u5c1a\u4e0d\u652f\u6301\u5355\u4e00\u7b7e\u540d\u63cf\u8ff0\u7b26", "\u5c1a\u672a\u5b9e\u65bd", "\u5d4c\u5957\u9694\u79bb\u89c1\u8bc1", "\u5de5\u4f5c\u8fdb\u5c55\u4e2d", "\u5de5\u5177", "\u5df2\u542f\u7528", "\u5df2\u7981\u7528", "\u5e9f\u5f03", "\u5e9f\u5f03\u5bc6\u7801\u77ed\u8bed", "\u5f00\u53d1\u4eba\u5458\u9009\u9879", "\u5f03\u7528\u52a9\u8bb0\u8bcd", "\u5fc5\u987b\u9009\u62e9", "\u60a8\u5c1a\u672a\u8f93\u5165\u5bc6\u7801\u77ed\u8bed\u3002", "\u60a8\u73b0\u5728\u53ef\u4ee5\u53d6\u51fa SD\u5361\u4e86", "\u60a8\u7684\u627e\u96f6", "\u6211\u77e5\u9053\u4e86", "\u6240\u9700", "\u6253\u8d4f", "\u6269\u5c55\u516c\u94a5\u8be6\u60c5", "\u626b\u63cf", "\u626b\u63cf SeedQR", "\u626b\u63cf\u4e00\u4e2a SeedQR", "\u626b\u63cf\u4e00\u4e2a\u4e8c\u7ef4\u7801", "\u626b\u63cf\u4e00\u4e2a\u52a9\u8bb0\u8bcd\u5907\u4efd"]
ASCII = ['OK', 'Cancel', 'Back', 'Next', 'Done', 'Retry', 'Skip', 'Settings']

import json, machine, vfs, os, time as t
import seedsigner_lvgl_screens as ss
ss.init(); ss.set_screensaver_timeout(0)
try: os.listdir('/sd')
except OSError: vfs.mount(vfs.VfsFat(machine.SDCard(slot=0, width=4)), '/sd')
loc='zh_Hans_CN'
p={f: open('/sd/'+loc+'/'+f,'rb').read() for f in json.loads(ss.locale_pack_files(loc))}
print('load', loc, ss.load_locale(loc, p))
def show(tag):
    s=ss.mem_stats()
    print('%-16s used=%2d%% max=%-6d frag=%2d%% big=%-7d' % (
        tag, s['lvgl_used_pct'], s['lvgl_max_used'], s['lvgl_frag_pct'], s['lvgl_free_biggest']))
def blist(title, lbls):
    ss.clear_result_queue()
    ss.button_list_screen({'top_nav': {'title': title, 'show_back_button': True,
        'show_power_button': False}, 'button_list': lbls}); t.sleep_ms(40)

show('baseline')
for i in range(0, len(VARIED), 8):
    blist(VARIED[i], VARIED[i:i+8])
    if (i//8) % 3 == 0: show('frag %d' % i)
show('FRAGMENTED')
print('unload ->', ss.unload_locale())
blist('Menu', ASCII)          # ASCII screen: deletes last CJK screen -> reaps CJK caches
show('after-unload(ascii)')
blist('Menu', ASCII); show('settled1')
blist('Menu', ASCII); show('settled2')
print('--- done ---')
