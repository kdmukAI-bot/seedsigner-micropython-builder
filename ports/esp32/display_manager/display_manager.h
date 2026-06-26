#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "locale_loader.h"   /* ss_pack_provider_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*display_manager_ui_callback_t)(void *ctx);

void init(void);
const char *run_screen(display_manager_ui_callback_t cb, void *ctx);

/* Switch the active i18n locale via the shared loader, wrapped in the LVGL-port
 * lock. `provider` acquires each pack file's bytes — on MicroPython the binding
 * supplies a provider backed by bytes read off the SD card in Python (the
 * ESP-IDF FAT stack can't be linked alongside MicroPython's own FAT VFS).
 * Returns true on full success; false if a pack is missing (the loader restores
 * the baked Western floor — a recoverable "fall back to English", not an error). */
bool dm_load_locale(const char *locale, ss_pack_provider_t provider, void *user);

/* Clear everything dm_load_locale installed and restore the baked Western floor. */
void dm_unload_locale(void);

/* Set the native screensaver idle timeout in milliseconds (0 disables it).
 * Wraps overlay_manager_set_screensaver_timeout() in the LVGL-port lock. */
void dm_set_screensaver_timeout(uint32_t ms);

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

#ifdef __cplusplus
}
#endif


#endif // DISPLAY_MANAGER_H
