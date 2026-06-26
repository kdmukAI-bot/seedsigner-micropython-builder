# Widget-churn fragmentation test (docs/font-memory-plan.md Task E).
# Re-renders the SAME CJK screen 60x. After render 1 the glyph caches are warm (same
# glyphs -> cache HITS, no new nodes), so renders 2..60 are PURE widget build/teardown
# churn through load_screen_and_cleanup_previous (build-new-before-delete-old).
#   big COLLAPSES  -> the leapfrog fragments the pool; destroy-old-first would help.
#   big HOLDS      -> widgets coalesce cleanly; the glyph cache is the sole fragmenter.
# HARD-RESET the board before running (so baseline is clean).
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
    print('%-14s used=%2d%% max=%-6d frag=%2d%% big=%-7d' % (
        tag, s['lvgl_used_pct'], s['lvgl_max_used'], s['lvgl_frag_pct'], s['lvgl_free_biggest']))

TITLE="tBtc"
LABELS=["tBtc", "tSats", "btc", "sats", "SD \u5361\u5df2\u79fb\u9664", "SD \u5361\u5df2\u63d2\u5165", "\u5ba1\u6838\u4ea4\u6613", "\u5ba1\u6838\u8be6\u60c5"]
def render():
    ss.clear_result_queue()
    ss.button_list_screen({'top_nav': {'title': TITLE, 'show_back_button': True,
        'show_power_button': False}, 'button_list': LABELS}); t.sleep_ms(40)
render(); show('warm(1)')
for i in range(2, 61):
    render()
    if i % 5 == 0: show('render %d' % i)
show('FINAL'); print('--- done ---')
