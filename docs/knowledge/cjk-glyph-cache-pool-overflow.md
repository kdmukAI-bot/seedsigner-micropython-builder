# CJK glyph-cache index overflows the LVGL internal pool → freeze/reboot

_Measured on the ESP32-P4 4.3" (WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43), 2026-06-25, debug build,
MicroPython v1.27.0 / LVGL 9.5.0 / ESP-IDF 5.5.1. Companion: `docs/font-memory-plan.md` (Tasks D/E +
decision record)._

## Symptom

Rendering screens with realistic CJK (`zh_Hans_CN`) content reboots the board. The serial log shows a
task watchdog abort, not a clean error:

```
E task_wdt: Task watchdog got triggered ... IDLE1 (CPU 1) did not reset ...
E task_wdt: CPU 1: mp_task
... core dump saved ... Rebooting ...
```

It is **content-dependent**: Latin and light CJK (a 3-button menu) render fine; a single content-heavy
CJK screen (or a few real ones in sequence) trips it.

## Root cause

The LVGL **builtin memory pool** (`CONFIG_LV_MEM_SIZE_KILOBYTES=128` on this board; a static internal-DRAM
array) holds the per-`(font, px)` **glyph/kerning cache index** — red-black-tree nodes from
`lv_cache_lru_rb` / `lv_rb`. Glyph *bitmaps* and complex-script A8 *masks* are already routed to PSRAM
(`route_draw_buffers_to_psram()`), but the **cache index nodes are not** — `lv_cache`/`lv_rb` expose no
allocator hook, so they stay in the internal pool.

Each distinct glyph rendered at a given role size adds a cache entry (an RB node + its data). Measured
cost (see below): **~290 bytes per distinct glyph across two roles (~145 B/glyph/role)**. The per-role
cache is capped at `SEEDSIGNER_TTF_CACHE_SIZE = 256` entries, and ~3–4 CJK text roles (body 18px, button
20px, large-button/title 23px, main-menu-title 26px) are live at once. The `zh_Hans_CN` translation corpus
has **391 distinct CJK glyphs**, so a thorough navigation fills multiple roles toward their 256 cap:
roughly `4 roles × 256 × 145 B ≈ 148 KB` of index — **more than the 128 KB pool**.

When `lv_malloc` finally returns NULL inside the pool, LVGL's `LV_ASSERT_MALLOC` fires. With the **default
halting `LV_ASSERT_HANDLER` (`while(1)`)**, the calling task (`mp_task`, which renders inline under the
LVGL-port lock) spins forever, starves the idle task, and the task watchdog reboots the board ~seconds
later. This is the failure the font-memory plan predicted.

## Evidence

`mem_stats()` (the Task D binding) reading the LVGL pool + ESP-IDF heaps on-device.

**Synthetic glyph sweep** — distinct glyphs pushed through button(20px)+title(23px); dead-linear climb,
`int_free` (ESP-IDF internal heap) flat the entire time → all growth is the LVGL pool, not the IDF heap
and not stb scratch:

| distinct glyphs | `lvgl_used_pct` | `lvgl_max_used` | `lvgl_free_biggest` | `int_free` |
|---|---|---|---|---|
| 0 (baseline) | 18 % | 25,484 | 104,408 | 176,347 |
| 112 | 45 % | 59,280 | 66,988 | 176,347 |
| 168 | 59 % | 75,256 | 48,364 | 176,347 |
| 224 | 74 % | 92,280 | 29,784 | 176,347 |
| 280 | 89 % | 109,372 | 11,268 | 176,347 |
| ~330 | 💥 watchdog reboot | | | |

**Realistic navigation** — real `zh_Hans_CN` strings: rendering just the `seed_views.py` screen family
(224 distinct glyphs across body/button/title) reached **81 % / `max_used` 100,976 B**, and the next
group overflowed.

### Crash backtraces (`addr2line` against the build `micropython.elf`)

Realistic crash — the failing allocation **is the cache index**:
```
run_screen → button_list_screen → button_list → button_ex
 → apply_button_label_layout → lv_text_get_size      (measuring a CJK label width)
  → lv_font_get_glyph_dsc → ttf_get_glyph_dsc_cb
   → lv_cache_acquire_or_create → tiny_ttf_glyph_cache_create_cb
    → ttf_get_glyph_pair_kerning_width → lv_cache_acquire_or_create
     → add_cb → alloc_new_node → lv_rb_insert → rb_create_node   💥 lv_rb.c:342
```
`lv_rb.c:342` is `LV_ASSERT_MALLOC(node->data)` — the assert right after the cache RB node's data alloc.

Synthetic crash — failing allocation was a widget **event node**:
```
button_ex → lv_event_add   💥 lv_event.c:163  (LV_ASSERT_NULL(dsc))
```

Both are the same class: pool exhausted → `lv_malloc` NULL → halting assert → `while(1)` → `task_wdt`.

## Non-obvious points

- **`int_free` stays flat.** The overflow is the LVGL pool's **cache index**, *not* the ESP-IDF heap and
  *not* the stb rasterization scratch. So routing stb scratch to PSRAM (the cheap "Task C" win) helps the
  transient peak but does **not** relieve this persistent overflow — necessary-but-insufficient alone.
- **The cheap cycling test lied.** Cycling locales on 3-button screens settled at a comfortable 35.6 %
  and looked safe — a **false negative**, because each screen warmed only ~9 distinct glyphs in 1–2 roles.
  Any font-budget test MUST render realistic, glyph-dense content (real translated strings / paragraphs),
  or it will under-warm the caches and miss the overflow. This is the trap to avoid in future measurement.
- **It's a clean malloc-fail-assert, not the `tiny_ttf` cache-spin bug** and not corruption. So reducing
  `SEEDSIGNER_TTF_CACHE_SIZE` is safe (fewer RB nodes = less pressure, no new failure mode).
- **Crash-hygiene (non-halting `LV_ASSERT_HANDLER`) is only partial.** `rb_create_node` has a graceful
  `return NULL` *below* the assert, so the cache path could degrade gracefully (skip caching that glyph) —
  but `lv_event_add` has no guard and would `NULL`-deref next. So a non-halting handler trades a 10 s
  freeze for a faster crash on the widget path, and graceful degradation only on the cache path.

## Why fragmentation accumulates and can't be recovered (2026-06-25)

The byte ceiling is only half the problem; the harder half is **fragmentation**, and a lever sweep nailed
down why it's structural:

- **The three tiny_ttf caches.** Each instance has glyph + draw_data (sized by `SEEDSIGNER_TTF_CACHE_SIZE`)
  + a **kerning cache sized by the fixed `CONFIG_LV_TINY_TTF_CACHE_KERNING_CNT=256`** that the cache-size
  cap never touched (so cap cuts only shrank 2 of 3 caches — explaining why cap=64/128 barely helped). The
  kerning cache is redundant under `LV_FONT_KERNING_NONE` (the advance is already in the glyph node), so
  `CONFIG_LV_TINY_TTF_CACHE_KERNING_CNT=0` (no-cache bypass) removes ~20 KB for free.
- **Widgets are innocent.** Re-rendering the *same* screen 60× (glyphs warm → only widget build/teardown,
  through the build-new-before-delete-old transition in `seedsigner.cpp` `load_screen_and_cleanup_previous`)
  leaves `big` rock-stable (~94 KB). Widgets coalesce cleanly — the transition order is **not** a
  fragmenter, and reordering it (destroy-old-first) would not help.
- **The cache nodes are the sole fragmenter, and freeing them does NOT recover.** Fragment hard, then
  `unload_locale()` + an ASCII render (reaps the CJK caches): freed **51 KB** (used 66% → 24%) but `big`
  stayed **32 K** and frag jumped to **67%**. The freed holes are small and interspersed with persistent
  state (Latin-floor caches, font structs, LVGL globals) that never moves → scattered, un-coalescable.
- **Root cause:** LVGL's TLSF builtin pool coalesces *adjacent* frees but **cannot compact** (can't
  relocate live blocks — raw pointers). Long-lived cache RB nodes interleaved with transient widgets
  permanently break coalescing, and dropping them just makes more small holes.

## Fix direction — RESOLVED: Approach A required

The cheap size-reducers (dedup, kerning=0, cap) are necessary-but-insufficient — they raise the ceiling
but can't stop the fragmentation. **Pool bump** only delays (fragmentation is unbounded); **flush** can't
recover (proven); **widget reorder** is moot (widgets innocent). So the cache RB nodes must be kept out of
the internal pool:

- **Approach A** — build-time patch to `lv_rb` / `lv_cache_lru_rb` routing the node allocations to
  `heap_caps_malloc(MALLOC_CAP_SPIRAM)`, **scoped to tiny_ttf's caches** (generic `lv_rb` vs. a custom
  cache class is the open design question). The widget-churn result is positive proof it works: the pool
  defragments fine when only widgets occupy it — which is what it becomes once the caches move to PSRAM.
- **Keep:** dedup + `CONFIG_LV_TINY_TTF_CACHE_KERNING_CNT=0` (cut absolute PSRAM cache load + RB depth).
- **Validate during A:** per-glyph RB traversal in PSRAM on the animation/scroll hot path (the one risk).

Full sweep + numbers: `docs/font-memory-plan.md` (Task D+E results + decision record).
