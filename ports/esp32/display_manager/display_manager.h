#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "locale_loader.h"   /* ss_pack_provider_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*display_manager_ui_callback_t)(void *ctx);

void init(void);
const char *run_screen(display_manager_ui_callback_t cb, void *ctx);

/* Push the next animated-QR frame into a live qr_display_screen, under the LVGL-port
 * lock (the live-push equivalent of run_screen; see the definition). */
void dm_qr_display_set_frame(const void *data, size_t len);

/* Switch the active i18n locale via the shared loader, wrapped in the LVGL-port
 * lock. `provider` acquires each pack file's bytes — on MicroPython the binding
 * supplies a provider backed by bytes read off the SD card in Python (the
 * ESP-IDF FAT stack can't be linked alongside MicroPython's own FAT VFS).
 * Returns true on full success; false if a pack is missing (the loader restores
 * the baked Western floor — a recoverable "fall back to English", not an error). */
bool dm_load_locale(const char *locale, ss_pack_provider_t provider, void *user);

/* Clear everything dm_load_locale installed and restore the baked Western floor. */
void dm_unload_locale(void);

/* --- Runtime language-pack discovery + locale-picker endonym images ---------
 * The language-selection screen (locale_picker_screen) and "copy a pack onto the
 * SD card, no firmware rebuild" flow. Like dm_load_locale, the per-host piece is
 * acquiring pack bytes: on MicroPython the binding reads the packs partition in
 * Python (machine.SDCard; the ESP-IDF FAT stack can't be linked alongside
 * MicroPython's own FAT VFS) and supplies the bytes through the seams below.
 * See docs/language-selection-integration-todo.md and the screens repo's
 * docs/knowledge/locale-picker-and-endonym-images.md. */

/* JSON manifest of every locale the firmware can render as a pack — compiled-in
 * fonts UNION runtime-registered SD packs — for the ACTIVE display profile (in
 * supported_locales_json() shape). A baked-floor Latin locale (English, ...) is
 * NOT listed (it needs no pack). Returns a pointer to a static buffer, valid
 * until the next call. Wraps the render-layer read in the LVGL-port lock. */
const char *dm_supported_locales_json(void);

/* Register a runtime language pack from its own manifest.json bytes (a pack whose
 * code is not compiled in), so ss_load_locale / dm_supported_locales_json then
 * serve it with no rebuild. FAILS CLOSED: returns false on malformed JSON or a
 * missing required field, registering nothing. See ss_register_pack_manifest(). */
bool dm_register_pack_manifest(const char *manifest_json, size_t len);

/* Drop every runtime-registered pack (e.g. before an SD rescan). */
void dm_clear_pack_manifests(void);

/* Point the locale picker at the byte provider it uses to fetch endonym images —
 * the SAME seam/signature as dm_load_locale's provider. Set before running
 * locale_picker_screen; pass NULL to disable image rows (they fall back to their
 * live text). See locale_picker_set_image_provider(). */
void dm_set_endonym_image_provider(ss_pack_provider_t provider, void *user);

/* Set the native screensaver idle timeout in milliseconds (0 disables it).
 * Wraps overlay_manager_set_screensaver_timeout() in the LVGL-port lock. */
void dm_set_screensaver_timeout(uint32_t ms);

/* Toast overlay (transient banner composited over the live screen via the top
 * layer). Flat args are marshaled into a toast_overlay_spec_t inside the .cpp, so
 * toast_overlay.h / overlay_manager.h stay out of the binding's QSTR-scan include
 * set (same header-hiding split as dm_mem_stats).
 *
 * dm_show_toast: raise or REPLACE the current toast. label_text is required (NULL
 * treated as empty); icon_glyph is a seedsigner-icon PUA glyph or NULL (text-only);
 * colors are 0xRRGGBB; duration_ms auto-dismisses (0 = stay until dismissed/replaced).
 * Wraps the thread-safe staging entry overlay_manager_show_toast().
 * dm_dismiss_toast: dismiss the current toast immediately (no-op if none). Wraps the
 * LVGL-thread-only toast_overlay_dismiss(). Both take the LVGL-port lock (mirroring
 * dm_set_screensaver_timeout), since the MicroPython task is the caller. */
void dm_show_toast(const char *label_text, const char *icon_glyph,
                   uint32_t outline_color, uint32_t font_color, uint32_t duration_ms);
void dm_dismiss_toast(void);

/* Memory instrumentation (docs/font-memory-plan.md, Task D). A plain-C snapshot
 * of the internal LVGL builtin pool and the ESP-IDF PSRAM/internal heaps, kept
 * free of lvgl.h / esp_heap_caps.h types so the MicroPython binding can consume
 * it without pulling those headers into its QSTR-scan include set. Sizes are in
 * bytes; the *_pct fields are 0..100. The *_min_free fields are each heap's
 * lowest-ever free size (i.e. its all-time high-water of use). */
typedef struct {
    /* LVGL builtin pool (internal DRAM; CONFIG_LV_MEM_SIZE_KILOBYTES). */
    uint32_t lvgl_total;
    uint32_t lvgl_free;
    uint32_t lvgl_free_biggest;
    uint32_t lvgl_max_used;
    uint8_t  lvgl_used_pct;
    uint8_t  lvgl_frag_pct;
    /* ESP-IDF heaps: free-now + minimum-ever-free (high-water). */
    uint32_t spiram_free;
    uint32_t spiram_min_free;
    uint32_t internal_free;
    uint32_t internal_min_free;
    /* rb-cache PSRAM routing (Approach A; docs/approach-a-cache-psram-design.md).
     * rb_psram_enabled: 1 if routing on. alloc/free_total: cumulative PSRAM rb-node
     * allocs/frees. live_nodes/live_bytes: currently held. fallback_total: PSRAM
     * alloc failures that fell back to the internal pool (should stay 0). */
    uint32_t rb_psram_enabled;
    uint32_t rb_psram_alloc_total;
    uint32_t rb_psram_free_total;
    uint32_t rb_psram_live_nodes;
    uint32_t rb_psram_live_bytes;
    uint32_t rb_psram_fallback_total;
} dm_mem_stats_t;

/* Fill *out with a memory snapshot. The LVGL pool read is wrapped in the
 * LVGL-port lock (lv_mem_monitor walks allocator state the esp_lvgl_port task
 * mutates concurrently); the heap_caps reads are already thread-safe. If the
 * lock is unavailable the lvgl_* fields are left zeroed but the heap fields are
 * still populated. Safe to call with out == NULL (no-op). */
void dm_mem_stats(dm_mem_stats_t *out);

/* Toggle rb-cache PSRAM routing at runtime (Approach A A/B measurement). Routing
 * defaults on; flip off early in a test script to reproduce the original in-pool
 * overflow as a control. Affects only subsequent allocations (existing PSRAM nodes
 * stay valid and free correctly). Wraps lv_rb_psram_set_enabled(). */
void dm_set_cache_psram(bool enabled);

/* Active display-profile size in pixels: writes active_profile().width to *width and
 * active_profile().height to *height.
 * A plain-C getter over the render layer's active_profile() so the MicroPython binding
 * can report the real panel dimensions without pulling gui_constants.h (C++) into its
 * QSTR-scan include set — the same header-hiding split as dm_mem_stats. The profile is
 * fixed at boot and immutable after, so no LVGL-port lock is needed. Each pointer is
 * written only when non-NULL (safe to pass NULL for a dimension you don't want). */
void dm_display_size(int *width, int *height);

#ifdef __cplusplus
}
#endif


#endif // DISPLAY_MANAGER_H
