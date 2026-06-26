/**
 * Display manager — thin MicroPython wrapper around board_common.
 *
 * All hardware init (I2C, display, touch, PMIC, backlight, LVGL port)
 * is handled by board_init(). This file just provides the init() and
 * run_screen() C functions that the MicroPython bindings call.
 */
#include <exception>

#include "board.h"
#include "board_backlight.h"
#include "board_config.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "draw/lv_draw_buf_private.h"  // lv_draw_buf_handlers_t fields (buf_malloc/free_cb)

#if BOARD_HAS_SDCARD && defined(CONFIG_IDF_TARGET_ESP32P4)
#include "esp_ldo_regulator.h"   // on-chip LDO that powers the SD card VDD
#endif

#include "display_manager.h"
#include "gui_constants.h"
#include "locale_loader.h"    // ss_load_locale / ss_unload_locale (i18n font packs)
#include "overlay_manager.h"  // overlay_manager_init / _set_screensaver_timeout

static const char *TAG = "display_manager";

// rb-cache PSRAM routing (Approach A; docs/approach-a-cache-psram-design.md).
// Implemented in the patched LVGL managed component (src/misc/lv_rb.c); declared
// here because they're not in any LVGL public header.
extern "C" {
    void lv_rb_psram_get_stats(uint32_t out[6]);  // [enabled,alloc,free,live_nodes,live_bytes,fallback]
    void lv_rb_psram_set_enabled(bool en);
    bool lv_rb_psram_is_enabled(void);
}

// ---------------------------------------------------------------------------
// Route LVGL DRAW BUFFERS (tiny_ttf glyph bitmaps + glyph-run masks) to PSRAM.
//
// LVGL here runs on its built-in fixed pool (LV_USE_BUILTIN_MALLOC, LV_MEM_SIZE
// = 64 KB of internal RAM). Two kinds of big bitmap allocations would otherwise
// land in that tiny pool, both via lv_draw_buf_create()'s handler tables:
//
//  1. tiny_ttf's glyph/draw cache (SEEDSIGNER_TTF_CACHE_SIZE = 256) holds rasterized
//     glyph bitmaps — allocated via the FONT handlers. Across several renders (e.g.
//     switching locales) they exhaust the 64 KB, the next lv_malloc returns NULL, and
//     LVGL's LV_ASSERT_MALLOC busy-loops — the "spin" that wedged the LVGL task on ru.
//
//  2. The complex-script render layer (glyph_runs.cpp) bakes each shaped label into
//     an A8 alpha mask via lv_draw_buf_create(), which uses the DEFAULT handlers. A
//     label keeps its mask for the screen's lifetime, so a 4-label screen holds 4
//     masks at once. Tall scripts make these huge: Urdu Nastaliq's diagonal baseline
//     cascade gives a line-height ~2.5x the em, so a single one-line title mask is
//     ~120 KB (vs ~45 KB for the same text in Devanagari). They never fit the 64 KB
//     pool, lv_draw_buf_create() returns NULL, bake_run() bails, and the label falls
//     back to UNSHAPED codepoint text — which for Nastaliq renders as mis-placed
//     "vertical bars" and (being far wider unshaped) triggers the title scroll anim.
//     This is why ur was the lone locale broken on-device while it renders correctly
//     on desktop/Pi-Zero (both back LVGL with unbounded malloc).
//
// The fix (per seedsigner-lvgl-screens docs/knowledge/tiny-ttf-cache-spin-root-
// cause.md): override BOTH handler tables' buf_malloc/free so these bitmaps come from
// the 32 MB PSRAM via heap_caps_malloc, instead of the tiny internal pool. Small cache
// nodes + stb scratch + LVGL objects stay in fast internal RAM. Must run after
// lv_init() (board_init).
static void *psram_draw_buf_malloc(size_t size, lv_color_format_t cf)
{
    LV_UNUSED(cf);
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_8BIT);  // fall back to internal
    return p;
}

static void psram_draw_buf_free(void *buf)
{
    heap_caps_free(buf);
}

static void route_draw_buffers_to_psram(void)
{
    // Font handlers: tiny_ttf glyph bitmaps (the ru-freeze fix).
    lv_draw_buf_handlers_t *fh = lv_draw_buf_get_font_handlers();
    fh->buf_malloc_cb = psram_draw_buf_malloc;
    fh->buf_free_cb = psram_draw_buf_free;

    // Default handlers: lv_draw_buf_create() — the glyph-run A8 masks (the ur fix).
    lv_draw_buf_handlers_t *dh = lv_draw_buf_get_handlers();
    dh->buf_malloc_cb = psram_draw_buf_malloc;
    dh->buf_free_cb = psram_draw_buf_free;

    ESP_LOGI(TAG, "tiny_ttf glyph bitmaps + glyph-run masks routed to PSRAM");
}

#if BOARD_HAS_SDCARD && defined(CONFIG_IDF_TARGET_ESP32P4)
// The P4 Touch LCD 4.3 powers the microSD card's VDD from on-chip LDO channel 4
// (LDO_VO4 → SDMMC IO), per the Waveshare BSP. MicroPython's machine.SDCard does
// NOT configure this rail, so without it the slot is unpowered and the card never
// enumerates (block count -1, writes fail, mkfs → EBUSY). Acquire LDO_VO4 at 3.3V
// once at boot and hold it for the device's lifetime so machine.SDCard works.
static esp_ldo_channel_handle_t s_sd_ldo = NULL;
static void sd_power_on(void)
{
    if (s_sd_ldo) return;
    esp_ldo_channel_config_t cfg = { .chan_id = 4, .voltage_mv = 3300 };
    esp_err_t err = esp_ldo_acquire_channel(&cfg, &s_sd_ldo);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD LDO power-on failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SD card VDD powered via on-chip LDO ch4 @3.3V");
    }
}
#else
static void sd_power_on(void) {}
#endif

static lv_display_t *lvgl_disp = NULL;
static lv_indev_t *lvgl_touch_indev = NULL;
static bool initialized = false;

extern "C" void init(void)
{
    if (initialized) return;
    board_app_config_t cfg = { .landscape = true };
    board_init(&cfg, &lvgl_disp, &lvgl_touch_indev);

    /* Route glyph bitmaps + draw-buf masks to PSRAM BEFORE set_display() (which
     * rasterizes the baked Western floor) so even those first bitmaps avoid the
     * 64 KB pool. */
    route_draw_buffers_to_psram();

    /* Approach A: tiny_ttf/image cache index nodes (the lv_rb tree nodes) are
     * routed to PSRAM in the patched LVGL component so they stop fragmenting the
     * internal pool under realistic CJK. Log the state for boot-log visibility. */
    ESP_LOGI(TAG, "rb-cache PSRAM routing (Approach A): %s",
             lv_rb_psram_is_enabled() ? "enabled" : "disabled");

    /* Select the display profile that matches this board's resolution.
     * Landscape mode swaps H/V: LVGL width = V_RES, height = H_RES. */
    set_display(BOARD_LCD_V_RES, BOARD_LCD_H_RES);

    /* Start the native overlay manager's dispatcher lv_timer now that the LVGL
     * display + touch indev exist. This runs once, here in init() — the same
     * boot-time, pre-REPL, single-threaded window as set_display() above — so it
     * needs no lvgl_port lock (init() is `initialized`-guarded, never re-entered
     * from the MicroPython task). The dispatcher then fires inside the esp_lvgl_port
     * task's lv_timer_handler() and owns the screensaver on its own. */
    overlay_manager_init();

    initialized = true;
}

/**
 * Called from MICROPY_BOARD_STARTUP (before REPL starts).
 * Initializes the display at C-level boot so the SPI restart
 * workaround (for ST7796 boards) happens invisibly during startup.
 */
extern "C" void boardctrl_startup(void);  /* original NVS/flash init */

extern "C" void seedsigner_board_startup(void)
{
    boardctrl_startup();
    init();

    /* Power the SD card's VDD rail so MicroPython's machine.SDCard can enumerate
     * it. The card is then mounted + read on the MicroPython side (its own FAT
     * VFS) — we deliberately do NOT link ESP-IDF's esp_vfs_fat/fatfs, which would
     * collide with MicroPython's oofatfs (duplicate f_mount/f_open/...). Pack
     * bytes reach the C font loader via dm_load_locale()'s provider. See
     * docs/knowledge/micropython-fatfs-vs-esp-idf-fatfs-collision.md and
     * docs/knowledge/esp32-p4-sdcard-ldo-power.md. */
    sd_power_on();

    /* Render black screen with backlight off, then turn on backlight.
     * This avoids a flash of LVGL's default white background — the
     * first visible frame is a clean black screen. */
    if (lvgl_disp && lvgl_port_lock(0)) {
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
#if SEEDSIGNER_DEBUG
        lv_obj_t *label = lv_label_create(lv_screen_active());
        lv_label_set_text(label, "device ready");
        lv_obj_set_style_text_color(label, lv_color_make(128, 128, 128), 0);
        lv_obj_center(label);
#endif
        lvgl_port_unlock();
    }
    /* Give LVGL time to flush the splash frame before backlight on */
    vTaskDelay(pdMS_TO_TICKS(100));
    board_backlight_set(100);
}

extern "C" const char *run_screen(display_manager_ui_callback_t cb, void *ctx)
{
    if (!cb) {
        return "null screen callback";
    }
    if (!lvgl_port_lock(0)) {
        return "display lock unavailable";
    }

    const char *err = NULL;
    try {
        cb(ctx);
    } catch (const std::exception &e) {
        err = e.what();
    } catch (...) {
        err = "unknown exception in screen callback";
    }

    lvgl_port_unlock();
    return err;
}

/* --- i18n locale / font-pack loading -------------------------------------
 * The shared loader (ss_load_locale) owns all orchestration: clear the previous
 * locale, register each role font at the right px, and — for complex scripts —
 * load runs.bin + install the glyph run table. The ONE per-host piece is
 * acquiring the pack bytes; the binding supplies a `provider` that serves them
 * (on the P4 it serves bytes the MicroPython side read off the SD card via its
 * own FAT VFS — see the boot-hook note above). On the real signing device this
 * provider seam is where pack-signature verification will live (locale_loader.h).
 *
 * These wrap the loader in the esp_lvgl_port lock: font registration rasterizes
 * via tiny_ttf, which mutates LVGL state, and the LVGL task runs on its own
 * FreeRTOS task here (unlike the single-threaded desktop/Pi pump). */

extern "C" bool dm_load_locale(const char *locale, ss_pack_provider_t provider, void *user)
{
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "dm_load_locale: display lock unavailable");
        return false;
    }
    bool ok = ss_load_locale(locale, provider, user);
    lvgl_port_unlock();
    if (!ok) {
        ESP_LOGW(TAG, "locale '%s' did not fully load; using baked Western floor",
                 locale ? locale : "(null)");
    }
    return ok;
}

extern "C" void dm_unload_locale(void)
{
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "dm_unload_locale: display lock unavailable");
        return;
    }
    ss_unload_locale();
    lvgl_port_unlock();
}

/* Set the screensaver idle timeout (ms; 0 disables). Called from Python on the
 * MicroPython task, concurrent with the esp_lvgl_port task — and a 0 here can
 * dismiss an active saver (loads/deletes screens), so take the LVGL-port lock,
 * exactly as dm_load_locale/dm_unload_locale do. */
extern "C" void dm_set_screensaver_timeout(uint32_t ms)
{
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "dm_set_screensaver_timeout: display lock unavailable");
        return;
    }
    overlay_manager_set_screensaver_timeout(ms);
    lvgl_port_unlock();
}

/* Memory instrumentation for the font-memory budget work (font-memory-plan.md,
 * Task D). The binding builds a dict from this plain-C struct; the lvgl.h /
 * esp_heap_caps.h dependency stays here rather than leaking into the bindings'
 * QSTR-scan include set. */
extern "C" void dm_mem_stats(dm_mem_stats_t *out)
{
    if (!out) {
        return;
    }
    *out = dm_mem_stats_t{};

    /* ESP-IDF heaps are internally locked, so read them without the LVGL lock. */
    out->spiram_free       = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->spiram_min_free   = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    out->internal_free     = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->internal_min_free = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    /* rb-cache PSRAM routing counters (Approach A) — plain volatile reads, no lock
     * needed; populated even if the LVGL lock below is unavailable. */
    uint32_t rb[6];
    lv_rb_psram_get_stats(rb);
    out->rb_psram_enabled        = rb[0];
    out->rb_psram_alloc_total    = rb[1];
    out->rb_psram_free_total     = rb[2];
    out->rb_psram_live_nodes     = rb[3];
    out->rb_psram_live_bytes     = rb[4];
    out->rb_psram_fallback_total = rb[5];

    /* lv_mem_monitor walks the LVGL builtin allocator, which the esp_lvgl_port
     * task mutates concurrently — take the same lock the other dm_* wrappers do.
     * If it's unavailable, the lvgl_* fields stay zeroed (heap fields above are
     * already filled). */
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "dm_mem_stats: display lock unavailable");
        return;
    }
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    lvgl_port_unlock();

    out->lvgl_total        = (uint32_t)mon.total_size;
    out->lvgl_free         = (uint32_t)mon.free_size;
    out->lvgl_free_biggest = (uint32_t)mon.free_biggest_size;
    out->lvgl_max_used     = (uint32_t)mon.max_used;
    out->lvgl_used_pct     = mon.used_pct;
    out->lvgl_frag_pct     = mon.frag_pct;
}

/* Runtime A/B toggle for rb-cache PSRAM routing (Approach A). See header. */
extern "C" void dm_set_cache_psram(bool enabled)
{
    lv_rb_psram_set_enabled(enabled);
    ESP_LOGI(TAG, "rb-cache PSRAM routing %s via dm_set_cache_psram",
             enabled ? "ENABLED" : "DISABLED");
}
