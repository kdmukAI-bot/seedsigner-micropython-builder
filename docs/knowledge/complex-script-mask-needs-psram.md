# ESP32-P4: route the glyph-run A8 masks to PSRAM too (Urdu "vertical bars" / scrolling title)

**TL;DR.** The complex-script render layer bakes each shaped label into an A8 alpha mask with
`lv_draw_buf_create()`, which uses LVGL's **default** draw-buf handlers → `lv_malloc` → the 64 KB
internal `LV_MEM` pool. Tall scripts make these masks huge (Urdu **Nastaliq** ≈ 120 KB for a single
one-line title), so they never fit the pool: `lv_draw_buf_create()` returns NULL, `bake_run()` bails,
and the label silently falls back to **unshaped** codepoint text — which for Nastaliq renders as
misplaced "vertical bars" and (being far wider unshaped) triggers the title scroll animation. The fix
is the symmetric companion to the glyph-cache fix: route the **default** draw-buf handlers to PSRAM,
not just the **font** handlers. See `ports/esp32/display_manager/display_manager.cpp`
(`route_draw_buffers_to_psram`).

## Symptom

- On the **ESP32-P4** only, Urdu (`ur`, Nastaliq, RTL) rendered as a row of evenly-spaced thin
  **vertical bars**, and the top-nav title **scrolled** (`LV_LABEL_LONG_SCROLL_CIRCULAR`).
- The **same** code + **same** `ur.ttf`/`runs.bin` (byte-for-byte; CRC-verified against the SD card)
  rendered Urdu **correctly** on the desktop screenshot generator, the WASM web runner, and the Pi
  Zero — including at the P4's exact 800×480 resolution.
- Every other locale was fine on the P4: `ru`/`zh`/`ja`/`ko` (CJK/Cyrillic), `fa` (Arabic via LVGL's
  native presentation-form path), and crucially `hi`/`th` (Devanagari/Thai — the **same glyph-run
  baking path** as `ur`).

## Why device-only, and why `ur` specifically

The desktop/Pi builds back LVGL with **unbounded** malloc (`LV_STDLIB_CLIB`). The P4 firmware runs
LVGL on `LV_USE_BUILTIN_MALLOC` with `LV_MEM_SIZE = 64 KB` of **internal** RAM. So memory pressure
that's invisible everywhere else is real on the P4.

The render path (`seedsigner-lvgl-screens/components/seedsigner/glyph_runs.cpp::bake_run`): each shaped
label is composited into one A8 mask sized to the font's line box plus a generous bearing/overhang
margin:

```
margin  = line_height
mask_w  = layout_w        + 2*margin
mask_h  = nlines*line_height + 2*margin
mask    = lv_draw_buf_create(mask_w, mask_h, LV_COLOR_FORMAT_A8, 0)
```

`lv_draw_buf_create()` allocates via the **default** draw-buf handlers (`lv_draw_buf_get_handlers()`),
whose default `buf_malloc` calls `lv_malloc` → the 64 KB pool.

Nastaliq's diagonal baseline cascade gives the font a **line-height ≈ 2.5× the em**, so a single
one-line `ur` title mask measured **122,820 bytes** (356×345) on-device — vs ~45 KB for the same UI
string in Devanagari. `ur`'s masks (~100–122 KB each) can **never** fit the 64 KB pool. And because
each label owns its mask for the screen's lifetime, a 4-label screen needs all four masks at once, so
even the smaller `hi`/`th` masks get squeezed out once the pool is partly used.

When `lv_draw_buf_create()` returns NULL, `bake_run()` returns NULL, the glyph run is **not attached**,
the label's `text_opa` stays visible, and LVGL draws the **raw codepoint text**. For Nastaliq that means
the Arabic presentation forms placed one-per-advance with no GPOS cascade — the "vertical bars" — and
since unshaped Nastaliq is far wider than the shaped cascade, it overflows the title region and scrolls.
(The scrolling title was the key tell: an *engaged* run sets `text_opa = TRANSP` and draws a static
mask — nothing to animate. A visibly scrolling title means the run did **not** engage.)

This is an OOM that fails **closed and silent** (graceful NULL from `lv_draw_buf_create`), unlike the
glyph-*cache* OOM which fails **loud** (`LV_ASSERT_MALLOC` → `while(1)` CPU hang). See
[`tiny-ttf-cache-needs-psram.md`](tiny-ttf-cache-needs-psram.md) and the shared
[`tiny-ttf-cache-spin-root-cause.md`](../../deps/seedsigner-lvgl-screens/docs/knowledge/tiny-ttf-cache-spin-root-cause.md).

## Three draw-buf handler tables — route the right ones

LVGL keeps **three** independent handler tables (`src/draw/lv_draw_buf.c`), all defaulting to
`lv_malloc`:

| getter | used by | what it allocates |
|---|---|---|
| `lv_draw_buf_get_font_handlers()` | tiny_ttf glyph cache | rasterized **glyph bitmaps** |
| `lv_draw_buf_get_handlers()` (DEFAULT) | `lv_draw_buf_create()` | the **glyph-run masks**, canvases, snapshots |
| `lv_draw_buf_get_image_handlers()` | image cache | decoded image buffers |

The original ru-freeze fix overrode only the **font** handlers. The glyph-run masks go through the
**default** handlers, which were still on the 64 KB pool — that's the gap this fix closes.

## The fix

`ports/esp32/display_manager/display_manager.cpp`, run once after `lv_init()` (in `init()`):

```c
static void *psram_draw_buf_malloc(size_t size, lv_color_format_t cf) {
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_8BIT);  // fall back to internal
    return p;
}
static void route_draw_buffers_to_psram(void) {
    lv_draw_buf_handlers_t *fh = lv_draw_buf_get_font_handlers();  // glyph bitmaps
    fh->buf_malloc_cb = psram_draw_buf_malloc; fh->buf_free_cb = psram_draw_buf_free;
    lv_draw_buf_handlers_t *dh = lv_draw_buf_get_handlers();       // glyph-run masks
    dh->buf_malloc_cb = psram_draw_buf_malloc; dh->buf_free_cb = psram_draw_buf_free;
}
```

Only `buf_malloc_cb`/`buf_free_cb` are overridden; `align_pointer_cb` etc. stay at the LVGL defaults
(heap_caps_malloc returns ≥ `LV_DRAW_BUF_ALIGN`-aligned memory, so the default `buf_align` is a no-op).
The display framebuffers are owned by `esp_lvgl_port`, not `lv_draw_buf_create`, so they are unaffected.
On the P4, PSRAM is DMA/PPA-readable, so masks composited from PSRAM draw correctly.

## Why keep the fix on the device side (not in the shared render layer)

`glyph_runs.cpp` is platform-agnostic and correct: a generous overhang margin is genuinely needed for
the Nastaliq cascade, and the masks render fine wherever LVGL is backed by adequate memory. The
device is the constrained target, so the device provisions the memory — exactly the pattern the
font-handler fix established. The shared repo stays clean.

## Diagnosis notes (how this was pinned down without guessing)

A temporary `SEEDSIGNER_GRUN_DEBUG` build dumped, per label, in `attach_runs`/`bake_run`:
- the by_text lookup **hit** (key/label bytes matched — ruled out a keying/encoding bug), and
- `bake_run`'s `mask=NULL` plus exact `mask_w/mask_h/bytes` — `ur` ~100–122 KB (NULL), `hi` ~34–45 KB.

After routing the default handlers to PSRAM the same labels reported `mask=OK`, glyph bitmaps `db=OK`,
and the P4 rendered `ur` as proper Nastaliq (webcam-verified), with `hi`/`th` unchanged.
