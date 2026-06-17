# ESP32-S3: route the tiny_ttf glyph cache to PSRAM (REQUIRED — cache is on by default)

**Required:** ensure the rasterized glyph bitmaps held by `lv_tiny_ttf`'s cache are allocated from
**PSRAM**, not the (small) internal LVGL/MP heap. The shared c-modules font code now enables the glyph
cache **by default** (`SEEDSIGNER_TTF_CACHE_SIZE = 256`), so a firmware that bumps the c-modules submodule
without PSRAM-backing the cache **will CPU-hang on CJK content**. This must land in lockstep with the
submodule pin.

## Background

The shared font code in c-modules (`components/seedsigner/font_registry.cpp`, `gui_constants.cpp`) creates
its tiny_ttf fonts with the glyph cache enabled by default. The reason it must be PSRAM-backed — fully
root-caused — is documented in the submodule:
[`deps/seedsigner-c-modules/docs/knowledge/tiny-ttf-cache-spin-root-cause.md`](../../deps/seedsigner-c-modules/docs/knowledge/tiny-ttf-cache-spin-root-cause.md).

Summary: with `cache_size > 0`, the draw cache holds rasterized CJK bitmaps. The cache is **count**-bounded
(128 entries), not byte-bounded, so it can hold hundreds of KB. On a small fixed heap that exhausts memory;
the next `lv_malloc` returns NULL and LVGL's default `LV_ASSERT_MALLOC` → `LV_ASSERT_HANDLER` = `while(1);`
spins the CPU. It is **not** an LVGL cache/red-black-tree bug and **not** upstream #9765 — it is plain OOM.
The stack is not OOM-safe (stb_truetype also `abort`s on its own failed malloc), so the fix is to **provide
enough memory**, specifically PSRAM, not to try to handle OOM.

## What this build already has

- `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_USE_MALLOC=y` — PSRAM is enabled and integrated into the heap allocator.
- **Caveat:** `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192` — allocations ≤ 8 KB are served from *internal*
  RAM by default. CJK A8 glyph bitmaps are only a few hundred bytes each, so they fall **under** this
  threshold and would land in internal RAM even though PSRAM is available. Plain `malloc`/`LV_STDLIB_CLIB`
  therefore does **not** automatically move glyph bitmaps to PSRAM.

## How tiny_ttf allocates (two sinks, both host-controlled)

1. **`lv_malloc`** (selected by `LV_USE_STDLIB_MALLOC`) — cache red-black-tree nodes + stb rasterizer
   scratch.
2. **`font_draw_buf_handlers`** (`LV_GLOBAL_DEFAULT()->font_draw_buf_handlers`, used by
   `lv_draw_buf_create_ex`) — the **glyph bitmaps** themselves (the large, cache-held allocations).

tiny_ttf has no "use PSRAM" flag; placement is decided entirely by how this firmware configures LVGL.

## Action items (to be done here, in the builder)

1. **Confirm current LVGL memory config:** which `LV_USE_STDLIB_MALLOC` the firmware compiles with
   (`LV_STDLIB_MICROPYTHON`? `BUILTIN`? `CLIB`?) and **where the MicroPython GC heap lives** (internal vs
   PSRAM). If LVGL uses the MP heap and that heap is in PSRAM, large allocations may already be covered —
   but glyph bitmaps are small (see the 8 KB caveat), so verify empirically.
2. **Route glyph bitmaps to PSRAM (recommended, targeted):** initialize
   `font_draw_buf_handlers` with PSRAM-backed alloc/free
   (`heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` / `heap_caps_free`). This forces the bitmaps to PSRAM
   regardless of the `ALWAYSINTERNAL` threshold, while small node/scratch allocations stay in fast
   internal SRAM.
   - Alternative (whole-heap): put the LVGL heap in PSRAM via `LV_STDLIB_BUILTIN` +
     `LV_MEM_ADR`/`LV_MEM_POOL_ALLOC`, or `LV_STDLIB_CUSTOM` wrapping `heap_caps_malloc(MALLOC_CAP_SPIRAM)`,
     or by ensuring the MP GC heap is in PSRAM under `LV_STDLIB_MICROPYTHON`.
3. **Crash-hygiene (optional, not a fix):** set a non-halting `LV_ASSERT_HANDLER` so a genuine OOM
   logs/reboots cleanly instead of freezing. This does **not** make OOM safe (stb still aborts); memory
   provisioning is the actual fix.
4. **Lockstep with the c-modules pin:** the cache is on by default in c-modules, so the PSRAM routing
   (this repo) must be in place at the same submodule bump. A genuinely memory-tight build could instead
   compile c-modules with `-DSEEDSIGNER_TTF_CACHE_SIZE=0`, but the intended path is PSRAM-backed cache.

## Note

This is open work in this repo: the c-modules cache is already on by default, so until the PSRAM routing
above is implemented and verified, do not ship a build that bumps the c-modules submodule with the cache
left on the internal heap — it will hang on CJK content.
