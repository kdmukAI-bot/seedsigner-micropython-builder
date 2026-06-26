# Font-memory plan — `seedsigner-micropython-builder`

_Status: planned, not yet implemented. Handoff doc so a new session can resume in this repo. The
companion doc lives in `seedsigner-lvgl-screens/docs/font-memory-plan.md` (font-instance dedup + CJK
size decisions). This repo owns: instrumentation, the stb→PSRAM build-time patch, data-gathering, and
the "is the invasive fix needed?" decision record._

## Why this exists

Glyph **bitmaps** and complex-script **A8 masks** are already routed to PSRAM
(`ports/esp32/display_manager/display_manager.cpp` → `route_draw_buffers_to_psram()`). What still taxes
the small **internal** LVGL builtin pool is: the per-`(font,px)` cache **index** nodes, the live widget
tree, and stb's transient rasterization **scratch**. The stack is **not OOM-safe** — a pool overflow
freezes (`LV_ASSERT_HANDLER` `while(1)`, confirmed default) → ~10 s task-watchdog panic
(`CONFIG_ESP_TASK_WDT_PANIC=y`) → reboot (`CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y`), or NULL-derefs →
instant reboot. No graceful degradation.

Pool sizes: **P4-43 = 128 KB** (`deps/micropython/mods/new_files/ports/esp32/boards/
WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/sdkconfig.board` → `CONFIG_LV_MEM_SIZE_KILOBYTES=128`). **S3 boards
= 64 KB** (no per-board setting → LVGL Kconfig default). P4 is the design center; **S3 is measure-only**.

Background/rationale: `seedsigner-lvgl-screens/docs/knowledge/tiny-ttf-cache-spin-root-cause.md`,
`docs/knowledge/tiny-ttf-cache-needs-psram.md`, `docs/knowledge/complex-script-mask-needs-psram.md`.

---

## Task C — stb scratch → PSRAM (build-time patch hook)

**Constraint that dictates the mechanism.** The firmware's LVGL is a **build-fetched ESP-IDF managed
component** (`lvgl/lvgl 9.5.0`, declared in the `0001` patch's `ports/esp32/main/idf_component.yml`),
materialized into the **gitignored** `deps/micropython/upstream/ports/esp32/managed_components/
lvgl__lvgl/`. It is **not** reachable by the existing source-patch flow
(`scripts/apply_micropython_mods.sh` / `generate_micropython_patch.sh` only diff `ports/`), and
`STBTT_malloc`/`STBTT_free` are *unconditional* `#define`s
(`managed_components/lvgl__lvgl/src/libs/tiny_ttf/lv_tiny_ttf.c` **L27–28**) so there is no `-D`
override. Hence: a build-time patch hook, not a source patch.

**The change (what the patch does).** Redirect L27–28 from `lv_malloc`/`lv_free` to
`heap_caps_malloc(x, MALLOC_CAP_SPIRAM)` (with an internal fallback for robustness) / `heap_caps_free`.
This moves the spiky, complexity-dependent rasterization scratch — and stb's own `abort()`-on-OOM path —
off the internal pool. Perf-neutral: rasterization is once-per-glyph (then cached), not a per-frame hot
path.

**How to deliver it (build-time hook):**
1. Carry a small, re-baseable unified diff, e.g. `deps/micropython/mods/component_patches/
   lvgl-stbtt-psram.patch` (targets `src/libs/tiny_ttf/lv_tiny_ttf.c` L27–28).
2. Add a CMake step — introduced **via the existing** `deps/micropython/mods/patches/
   0001-esp32-integration-mods.patch` (it already edits `ports/esp32/esp32_common.cmake` and
   `ports/esp32/main/CMakeLists.txt`) — that applies the patch to the fetched managed component **after**
   IDF component resolution and **before** the `lvgl` component compiles.
3. **Idempotent** (stamp file or `patch -p1 --forward`): the managed component is re-fetched on clean
   builds, and `apply_micropython_mods.sh` resets the tree, so the hook must tolerate re-runs.
4. Document that this **collapses to a one-line `target_compile_definitions(... -DSTBTT_malloc=...)`** if
   the macro is ever `#ifndef`-guarded upstream (see Task F / future PR).

No precedent exists in this repo for patching a managed component — this establishes the pattern; keep it
small and well-commented for re-basing on the next LVGL bump.

---

## Task D — `mem_stats()` MicroPython binding (instrumentation)

Add a native function returning LVGL pool + ESP-IDF heap stats so the high-water mark can be read
on-device. **No CMake change needed** — the binding file is already in `target_sources`.

- File: `bindings/modseedsigner_bindings.c`. Follow the existing `poll_for_result` pattern (~L263–282)
  for returning structured data; register in `seedsigner_lvgl_module_globals_table` (~L385) as
  `get_memory_stats` / `mem_stats`.
- Includes are reachable: `lvgl.h` / `lv_mem.h` (via the `display_manager` include path in
  `bindings/micropython.cmake`) and `esp_heap_caps.h` (already used in `ports/esp32/board_common/src/`).
- Return a dict with: LVGL pool from `lv_mem_monitor()` (`total_size`, `used_pct`, `max_used`,
  `free_size`, `free_biggest_size`, `frag_pct`) **plus** `heap_caps_get_free_size` /
  `heap_caps_get_minimum_free_size` for `MALLOC_CAP_SPIRAM` and `MALLOC_CAP_INTERNAL`.
- Invoke from the deployed app over serial (`tools/deploy_app.py` → REPL; the app imports
  `seedsigner_lvgl_screens`). Log the high-water after each heavy screen and after locale switches.
- Keep logging in debug builds permanent so each newly-ported screen self-reports a regression.

---

## Task E — data-gathering methodology + synthetic worst case

The ported screen set is **incomplete**, so today's heaviest real screen (settings list) is only a
*lower bound*. Bound both pool tenants:

- **Cache index** — screen-independent: `Σ(instances) × 256 × ~136 B`; per-view working set bounded by
  the closed translation corpus. Measurable now.
- **Widget tree** — the unknown tail. Build a **synthetic stress screen** (max-item `button_list_screen`
  under the densest CJK locale `zh_Hans`) to upper-bound it without waiting for ports.

**Worst-path procedure:** densest locale → navigate broadly to warm the body/button/title caches →
open the longest list → reproduce the original status-screen → large-button-list path → **capture the
locale-switch transient** (old + new locale fonts briefly coexist via the retire/reap path in
`font_registry.cpp`). Read `mem_stats()` at each step; `max_used` is the key number.

**Heaviest unported screens** (synthetic targets / port-priority, from the Python app `src/seedsigner/`):
1. `PSBTOverviewScreen` (`gui/screens/psbt_screens.py`) — custom chart, multi-field.
2. `SeedAddressVerificationScreen` (`gui/screens/seed_screens.py`) — live counter + monospace address.
3. `PSBTAddressDetailsScreen` / `PSBTChangeDetailsScreen` — long monospace addresses + derivation paths.
4. `ToolsAddressExplorerAddressListView` (`views/tools_views.py`) — long scrollable list (widget count).
5. `SeedMnemonicEntryView` (`views/seed_views.py`) — keyboard (widget count; baked ASCII keys = low
   cache).
6. Dense warning/educational text (e.g. `SeedWordsWarningView`) — highest CJK font-cache load per view.

**Thresholds:** target `max_used` ≤ ~70–75 % of pool; watch `free_biggest_size` and `frag_pct`
(fragmentation can crash with free bytes remaining). Record Latin baseline vs CJK delta, P4 vs S3.

**S3 measure-only:** run the same on the S3 (64 KB) for visibility; make **no** S3-specific change and do
not let S3 numbers drive the CJK cuts.

---

## Task D + E — RESULTS (measured 2026-06-25, P4-43, debug build)

`mem_stats()` (Task D) shipped and was exercised on-device over the REPL.

### Leak-free, bounded switching (good news)
- **Latin baseline:** `max_used` 25,484 B (20 %), frag 1 %, biggest-free 106 KB.
- **Locale cycling** (en/ru/zh_Hans_CN/th/hi/fa/ur × 3 passes, 3-button screens): **converged** — PSRAM
  high-water (`spiram_min_free`) floored in pass 1 and held **byte-stable** across passes 2–3; `max_used`
  ~45 KB (35.6 %); frag oscillated 1–17 % with **no drift**; biggest-free always ≥ 82 KB. **No leak**
  across heterogeneous (different-size) locale switches — the retire/reap path is sound.
- **Same-locale reload:** bounded ~111 KB PSRAM transient, fully reclaimed on the next render (no leak —
  just avoidable churn; the app should skip `load_locale` when the locale is already active).
- **Screensaver enter/dismiss:** memory-neutral (~11 KB PSRAM, no frag change).

### The pool overflows under realistic CJK (the headline)
The 35.6 % cycling figure was a **false negative**: 3-button screens warm only ~9 distinct glyphs across
1–2 roles. Rendering *realistic* CJK content **crashes the P4**:

- **Synthetic glyph sweep** (distinct glyphs through button 20px + title 23px): `max_used` climbs **dead
  linear** at **~290 B / distinct glyph** (≈ 145 B/glyph/role — matches the ~136 B estimate). Crosses the
  75 % target at ~210–225 glyphs; **overflows → reboot at ~330 glyphs**. `int_free` (ESP-IDF internal
  heap) stays **flat** the whole time — every byte of growth is the **LVGL-pool cache index**, not the IDF
  heap and not the stb scratch.
- **Realistic navigation** (real `zh_Hans_CN` strings grouped by source screen): rendering **just
  `seed_views.py`** (224 distinct glyphs across body/button/title) hit **81 % / `max_used` 100,976 B**,
  then the next screen group overflowed → reboot.
- `zh_Hans_CN` corpus = **391 distinct CJK glyphs**; with the 256/role cap and ~3–4 CJK roles active, the
  working set cannot fit 128 KB.

### Crash mechanism (core-dump backtraces, `addr2line` vs the build ELF)
Both crashes: `lv_malloc` fails in the LVGL pool → `LV_ASSERT_MALLOC`/`LV_ASSERT_NULL` → default
`LV_ASSERT_HANDLER` `while(1)` → `mp_task` spins → `IDLE` starved → `task_wdt` → reboot (the predicted chain).
- **Realistic:** the failing alloc was the **glyph/kerning cache RB node** —
  `lv_text_get_size → ttf_get_glyph_dsc_cb → lv_cache_acquire_or_create → lv_rb_insert → rb_create_node`
  (`lv_rb.c:342`, `LV_ASSERT_MALLOC(node->data)`). That is **the cache-index tenant itself**.
- **Synthetic:** the failing alloc was a **widget event node** — `button_ex → lv_event_add`
  (`lv_event.c:163`, `LV_ASSERT_NULL(dsc)`).
- The spin is a clean malloc-fail-assert, **not** the `tiny_ttf` cache-spin bug → a cache-cap cut is safe.
- Task G note: `rb_create_node` has a graceful `return NULL` *below* the assert, so a non-halting handler
  would degrade the cache path gracefully — but `lv_event_add` would `NULL`-deref next. Crash-hygiene is
  partial, as stated.

Full root-cause write-up: `docs/knowledge/cjk-glyph-cache-pool-overflow.md`.

### Cap-cut experiment (`SEEDSIGNER_TTF_CACHE_SIZE` 256 → 64) — INSUFFICIENT
Rebuilt the P4 with cap=64 and re-ran the realistic navigation. It avoided the immediate crash but still
climbed into the danger zone — **the cheap cap lever alone does not fit**:

| point | cap=256 | cap=64 |
|---|---|---|
| `seed_views.py` complete | 81 % / max 100,976 → **crash** | 65 % / max 83,248 (survived) |
| `settings_definition.py` (group 2 of 16) | — | 74 % / max 95,224 / **frag 29 % / big 21,776** → safe-guard stop (screen 18/61) |

Two reasons it fell short:
1. **~10 cache instances, not 4.** Peak ~95 KB ≈ `10 × 64 × 145 B`. Non-Latin locales keep the **Latin
   floor loaded as fallback** (5 CJK roles + 5 OpenSans-floor roles), and a **kerning-pair cache** is
   created per font (seen in the crash backtrace) despite kerning being nominally `KERNING_NONE`. The cap
   multiplies across all of them.
2. **Fragmentation became the limiter.** Small caps churn (constant eviction) → `frag` spiked to 29 % and
   `big` collapsed to ~22 KB at only 74 % used. Shrinking the cap trades bytes for fragmentation — **there
   is no cap sweet spot** that survives a full navigation.

→ The real lever is **instance-count reduction** (Task A dedup, in `seedsigner-lvgl-screens`): collapse role
px sizes, drop redundant Latin-floor instances, and kill the spurious kerning cache. Approach A remains the
escalation if dedup + cap still overflow. (cap=64 stays as a *contributing* lever — parameterized via
`-DSEEDSIGNER_TTF_CACHE_SIZE=N` — to sweep back up after dedup.)

### Dedup + kerning-cache + the fragmentation verdict

Continuing the sweep past the cap-cut experiment (above):

- **dedup (screens Task A, submodule bumped to `feat/font-memory-dedup` @ afd45ba):** shares tiny_ttf
  instances across roles with equal (weight, px). At the P4-43 profile the CJK roles mostly do NOT share,
  so it saved only ~3 KB (seed_views 100,976 → 97,604). Correct and free; keep it.
- **kerning cache — the dominant lever (~20 KB).** tiny_ttf has THREE internal RB caches per instance:
  glyph + draw_data (sized by `SEEDSIGNER_TTF_CACHE_SIZE`) + **kerning, sized by the fixed
  `CONFIG_LV_TINY_TTF_CACHE_KERNING_CNT=256`** — which the cap never touched (this is why cap=64/128 barely
  helped: they shrank 2 of 3 caches). Under `LV_FONT_KERNING_NONE` the glyph-dsc path *still* routes the
  per-glyph default advance through the kerning cache (`lv_tiny_ttf.c`:
  `if(dsc->kerning != LV_FONT_KERNING_NORMAL) ttf_get_glyph_pair_kerning_width(...)`) even though the
  advance is already stored in the glyph cache node (`node->adv_w`) — pure duplication.
  `CONFIG_LV_TINY_TTF_CACHE_KERNING_CNT=0` (board sdkconfig) hits tiny_ttf's no-cache bypass → seed_views
  97,604 → **77,984 (78% → 59%)**, zero functional change. **Keep permanently.**
- **Still short:** kerning=0 + dedup + cap=256 reached only **screen 30 of 61** (clean relaxed-guard run),
  peaking 90% / max 110 K / **frag 64% / big 4.5 KB**. From a clean start `big` collapsed monotonically
  across groups (108 K → 50 K → 22 K → 15 K → 4.5 K). It does not complete a full navigation.

**Why no cheap lever finishes — fragmentation is structural (two isolation tests, current firmware, no rebuild):**
- **Widgets are innocent.** Re-rendering the SAME CJK screen 60× (glyphs warm → no new cache nodes, only
  widget build/teardown via `load_screen_and_cleanup_previous`'s build-new-before-delete-old) left `big`
  rock-stable (~94 KB), `max_used` +1 KB total. So the transition ORDER is NOT a fragmenter; a
  destroy-old-first reorder would not help.
- **Cache nodes fragment AND can't be cleaned in place.** Fragment hard (15 CJK screens → big 33.6 K),
  then `unload_locale()` + an ASCII-only render to reap the CJK caches: it freed **51 KB** (used 66% →
  24%) but `big` stayed **32 K** and frag jumped to **67%**. The freed holes are small and interspersed
  with persistent state (Latin floor caches, font structs, LVGL globals) → scattered swiss-cheese, no
  coalescing. **A flush-at-safe-points strategy cannot recover contiguous space.**

LVGL's builtin pool (TLSF) coalesces *adjacent* frees but **cannot compact** (it can't relocate live
blocks — raw pointers). The long-lived cache RB nodes interleaved with transient widgets permanently block
coalescing → the only clean fix is to keep them out of the pool (see decision record).

**S3:** not yet measured; the P4 result already decides direction.

---

## Task F — upstream research

Research whether routing the tiny_ttf scratch (or cache) off the internal pool / making its allocator
overridable has been discussed or proposed. Scope: **lvgl/lvgl** (primary — where the code lives),
**micropython/micropython**, **lvgl/lv_binding_micropython**. Use the `deep-research` skill; record
findings here to inform the future PR decision (the build-time patch is structured to collapse to a `-D`
flag if upstream guards the macro).

---

## Task G — optional crash-hygiene (note, not required)

A non-halting `LV_ASSERT_HANDLER` (via `CONFIG_LV_ASSERT_HANDLER`) converts the 10 s freeze into a clean
logged reboot. **Not a fix** (stb still `abort()`s); document only.

---

## Decision record — RESOLVED 2026-06-25: Approach A IS required

The full lever sweep exhausted every cheap option and proved the fragmentation is **structural**, so
**Approach A (route the cache index nodes to PSRAM) is required.**

- **Cheap size-reducers — KEEP, but insufficient.** dedup (~3 KB) + kerning-cache removal (~20 KB, the big
  one) + cap moved the wall from "crash at group 1" to "screen 30 of 61," but never to the finish. They
  cut the cache *size* (raising the ceiling); they don't stop fragmentation.
- **Pool bump — REJECTED (delays only).** The ~176 KB free internal heap is the system's working reserve
  (driver/SD/camera transients, FreeRTOS stacks) — we can borrow some, not all. And fragmentation
  accumulates without bound over a long session, so any fixed pool size is just a longer fuse —
  unacceptable for a device left running.
- **Flush-at-safe-points — REJECTED (can't recover).** Freeing 51 KB of caches left `big` unchanged at
  32 K (scattered holes); TLSF can't compact. Proven dead.
- **Widget-reorder (destroy-old-first) — REJECTED (widgets are innocent).** Pure widget churn doesn't
  fragment; the build-new-before-delete-old order is not the cause.

So the cache RB nodes must not live in the internal pool. **Approach A:** build-time patch to
`lv_rb` / `lv_cache_lru_rb` routing the node allocations to `heap_caps_malloc(MALLOC_CAP_SPIRAM)` instead
of `lv_malloc`, **scoped to tiny_ttf's caches** (the main design question — generic `lv_rb` vs. a custom
cache class). The widget-churn test is positive proof it works: the pool defragments perfectly when only
widgets occupy it — which is what it becomes once the caches move to PSRAM.

**Keep regardless:** dedup + `CONFIG_LV_TINY_TTF_CACHE_KERNING_CNT=0` (they cut the absolute PSRAM cache
load and the per-lookup RB depth). **Open risk to validate during A:** per-glyph RB traversal in PSRAM on
the animation/scroll hot path (the plan's one standing concern about A). Byte-bounding the cache still does
not relieve internal RAM and is not pursued. Per the **P4-design-center** framing, the S3 must not drive any of this.

Root-cause detail: `docs/knowledge/cjk-glyph-cache-pool-overflow.md`. Auto-memory: `project_font_memory_budget`.

---

## Sequencing & cross-repo coordination

1. Land **screens Task A** (dedup) in `seedsigner-lvgl-screens`.
2. Here: **C** (stb hook) · **D** (binding) · **F** (research) — parallel, no compromises.
3. Bump the `seedsigner-lvgl-screens` submodule, build P4 (+S3), run **E** (measure).
4. Back to screens for **B-trivial**, re-measure; only then **B-bigger** if still over margin.
5. Write the **decision record** above.

## Verification

- **Build:** `BOARD=WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43 make docker-build-all` (and an S3 board) — see
  `/esp-build`. Confirm the stb patch applied (build log / stamp) and that a clean rebuild re-applies it
  idempotently.
- **stb routing:** `mem_stats()` peak during a first-paint CJK screen drops vs pre-patch; PSRAM
  `get_minimum_free_size` shows the scratch landing in PSRAM.
- **Dedup:** fewer live instances (instance-count log or `max_used` delta on a warmed CJK screen); no
  double-free / crash on locale switch + screen teardown.
- **Budget:** drive the Task-E worst path on the P4; `max_used` ≤ target with margin, no freeze/reboot;
  capture the locale-switch transient; record P4 vs S3 and Latin vs CJK.
- **Functional:** webcam-verify CJK + a shaping locale (`ur`) still render correctly (`/verify`,
  `/hw-debug`).
