#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "display_manager.h"
#include "locale_loader.h"   // ss_locale_pack_files / ss_pack_provider_t
#include "seedsigner.h"

#define SEEDSIGNER_RESULT_QUEUE_CAP 16
// 256 holds a full BIP39 passphrase (spec max) when a text-entry screen
// reports its result through this queue; also bounds button labels.
#define SEEDSIGNER_RESULT_LABEL_MAX 256

// What produced a queued result. The Python side reads this as the first
// element of the poll tuple and branches on it.
typedef enum {
    SEEDSIGNER_EVENT_BUTTON_SELECTED,
    SEEDSIGNER_EVENT_TEXT_ENTERED,
} seedsigner_result_kind_t;

typedef struct {
    seedsigner_result_kind_t kind;
    uint32_t index;
    char label[SEEDSIGNER_RESULT_LABEL_MAX];
} seedsigner_result_event_t;

static seedsigner_result_event_t s_result_queue[SEEDSIGNER_RESULT_QUEUE_CAP];
static uint32_t s_result_head = 0;
static uint32_t s_result_tail = 0;
static uint32_t s_result_count = 0;

static void seedsigner_result_enqueue(seedsigner_result_kind_t kind, uint32_t index, const char *label) {
    seedsigner_result_event_t ev = {
        .kind = kind,
        .index = index,
        .label = {0},
    };

    if (label) {
        strncpy(ev.label, label, SEEDSIGNER_RESULT_LABEL_MAX - 1);
        ev.label[SEEDSIGNER_RESULT_LABEL_MAX - 1] = '\0';
    }

    if (s_result_count == SEEDSIGNER_RESULT_QUEUE_CAP) {
        s_result_head = (s_result_head + 1) % SEEDSIGNER_RESULT_QUEUE_CAP;
        s_result_count--;
    }

    s_result_queue[s_result_tail] = ev;
    s_result_tail = (s_result_tail + 1) % SEEDSIGNER_RESULT_QUEUE_CAP;
    s_result_count++;
}

void seedsigner_lvgl_on_button_selected(uint32_t index, const char *label) {
    seedsigner_result_enqueue(SEEDSIGNER_EVENT_BUTTON_SELECTED, index, label);
}

// Override the weak default in seedsigner.cpp: a text-entry screen (e.g.
// seed_add_passphrase_screen) calls this on confirm with the entered text.
// Route it through the same queue so one poll loop sees both the confirmed
// text and a top-nav back-button press.
void seedsigner_lvgl_on_text_entered(const char *text) {
    seedsigner_result_enqueue(SEEDSIGNER_EVENT_TEXT_ENTERED, 0, text);
}

static void vstr_add_json_escaped(vstr_t *v, const char *src, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        char c = src[i];
        switch (c) {
            case '\\': vstr_add_str(v, "\\\\"); break;
            case '"': vstr_add_str(v, "\\\""); break;
            case '\n': vstr_add_str(v, "\\n"); break;
            case '\r': vstr_add_str(v, "\\r"); break;
            case '\t': vstr_add_str(v, "\\t"); break;
            // Append the RAW byte. NB: vstr_add_char() UTF-8-*encodes* its arg as a
            // codepoint on a unicode build, so feeding it raw UTF-8 bytes one at a
            // time re-encodes each byte as a Latin-1 codepoint (设 -> "è®¾"). The
            // source is already UTF-8; copy it through verbatim.
            default: vstr_add_byte(v, (byte)c); break;
        }
    }
}

static void vstr_add_json_from_obj(vstr_t *v, mp_obj_t obj);

static void vstr_add_json_from_dict(vstr_t *v, mp_obj_t obj) {
    mp_obj_dict_t *d = MP_OBJ_TO_PTR(obj);
    vstr_add_char(v, '{');

    bool first = true;
    mp_map_t *m = &d->map;
    for (size_t i = 0; i < m->alloc; ++i) {
        mp_map_elem_t *e = &m->table[i];
        if (e->key == MP_OBJ_NULL) {
            continue;
        }

        if (!first) {
            vstr_add_char(v, ',');
        }
        first = false;

        size_t klen = 0;
        const char *k = mp_obj_str_get_data(e->key, &klen);
        vstr_add_char(v, '"');
        vstr_add_json_escaped(v, k, klen);
        vstr_add_str(v, "\":");
        vstr_add_json_from_obj(v, e->value);
    }

    vstr_add_char(v, '}');
}

static void vstr_add_json_from_array(vstr_t *v, mp_obj_t obj) {
    size_t len = 0;
    mp_obj_t *items = NULL;
    mp_obj_get_array(obj, &len, &items);

    vstr_add_char(v, '[');
    for (size_t i = 0; i < len; ++i) {
        if (i) {
            vstr_add_char(v, ',');
        }
        vstr_add_json_from_obj(v, items[i]);
    }
    vstr_add_char(v, ']');
}

static void vstr_add_json_from_obj(vstr_t *v, mp_obj_t obj) {
    if (obj == mp_const_none) {
        vstr_add_str(v, "null");
        return;
    }

    if (obj == mp_const_true) {
        vstr_add_str(v, "true");
        return;
    }
    if (obj == mp_const_false) {
        vstr_add_str(v, "false");
        return;
    }

    if (mp_obj_is_int(obj)) {
        long val = mp_obj_get_int(obj);
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", val);
        vstr_add_str(v, buf);
        return;
    }

    if (mp_obj_is_str(obj)) {
        size_t len = 0;
        const char *s = mp_obj_str_get_data(obj, &len);
        vstr_add_char(v, '"');
        vstr_add_json_escaped(v, s, len);
        vstr_add_char(v, '"');
        return;
    }

    if (mp_obj_is_type(obj, &mp_type_dict)) {
        vstr_add_json_from_dict(v, obj);
        return;
    }

    if (mp_obj_is_type(obj, &mp_type_list) || mp_obj_is_type(obj, &mp_type_tuple)) {
        vstr_add_json_from_array(v, obj);
        return;
    }

    // Unknown/unsupported MicroPython object -> null.
    vstr_add_str(v, "null");
}

static mp_obj_t mp_seedsigner_lvgl_main_menu_screen(mp_obj_t cfg_obj) {
    // Display values are ALWAYS supplied by the caller: the app (Python/MicroPython) does the
    // gettext translation -- falling back to the English msgid -- and passes the result in. So
    // the contract REQUIRES a dict (localized top_nav.title + 4 button_list labels), exactly like
    // button_list_screen. The C side keeps internal defaults only as a per-key safety net.
    if (!mp_obj_is_type(cfg_obj, &mp_type_dict)) {
        mp_raise_TypeError(MP_ERROR_TEXT("main_menu_screen expects a dict"));
    }
    vstr_t json;
    vstr_init(&json, 256);
    vstr_add_json_from_obj(&json, cfg_obj);
    const char *err = run_screen(main_menu_screen, (void *)json.buf);
    vstr_clear(&json);
    if (err) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("%s"), err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(seedsigner_lvgl_main_menu_screen_obj, mp_seedsigner_lvgl_main_menu_screen);

static mp_obj_t mp_seedsigner_lvgl_screensaver_screen(void) {
    const char *err = run_screen(screensaver_screen, NULL);
    if (err) {
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s"), err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_screensaver_screen_obj, mp_seedsigner_lvgl_screensaver_screen);

static mp_obj_t mp_seedsigner_lvgl_button_list_screen(mp_obj_t cfg_obj) {
    if (!mp_obj_is_type(cfg_obj, &mp_type_dict)) {
        mp_raise_TypeError(MP_ERROR_TEXT("button_list_screen expects a dict"));
    }

    // Pass JSON through mostly unchanged and let screen-side C++ validate.
    vstr_t json;
    vstr_init(&json, 256);
    vstr_add_json_from_obj(&json, cfg_obj);

    const char *err = run_screen(button_list_screen, (void *)json.buf);

    vstr_clear(&json);

    if (err) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("%s"), err);
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(seedsigner_lvgl_button_list_screen_obj, mp_seedsigner_lvgl_button_list_screen);

static mp_obj_t mp_seedsigner_lvgl_large_icon_status_screen(mp_obj_t cfg_obj) {
    if (!mp_obj_is_type(cfg_obj, &mp_type_dict)) {
        mp_raise_TypeError(MP_ERROR_TEXT("large_icon_status_screen expects a dict"));
    }

    // Pass JSON through mostly unchanged and let screen-side C++ validate.
    vstr_t json;
    vstr_init(&json, 256);
    vstr_add_json_from_obj(&json, cfg_obj);

    const char *err = run_screen(large_icon_status_screen, (void *)json.buf);

    vstr_clear(&json);

    if (err) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("%s"), err);
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(seedsigner_lvgl_large_icon_status_screen_obj, mp_seedsigner_lvgl_large_icon_status_screen);

static mp_obj_t mp_seedsigner_lvgl_seed_add_passphrase_screen(mp_obj_t cfg_obj) {
    if (!mp_obj_is_type(cfg_obj, &mp_type_dict)) {
        mp_raise_TypeError(MP_ERROR_TEXT("seed_add_passphrase_screen expects a dict"));
    }

    // Pass JSON through mostly unchanged and let screen-side C++ validate.
    vstr_t json;
    vstr_init(&json, 256);
    vstr_add_json_from_obj(&json, cfg_obj);

    const char *err = run_screen(seed_add_passphrase_screen, (void *)json.buf);

    vstr_clear(&json);

    if (err) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("%s"), err);
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(seedsigner_lvgl_seed_add_passphrase_screen_obj, mp_seedsigner_lvgl_seed_add_passphrase_screen);

static mp_obj_t mp_seedsigner_lvgl_poll_for_result(void) {
    if (s_result_count == 0) {
        return mp_const_none;
    }

    seedsigner_result_event_t ev = s_result_queue[s_result_head];
    s_result_head = (s_result_head + 1) % SEEDSIGNER_RESULT_QUEUE_CAP;
    s_result_count--;

    qstr kind = (ev.kind == SEEDSIGNER_EVENT_TEXT_ENTERED)
        ? MP_QSTR_text_entered
        : MP_QSTR_button_selected;

    mp_obj_t out[3];
    out[0] = MP_OBJ_NEW_QSTR(kind);
    out[1] = mp_obj_new_int_from_uint(ev.index);
    out[2] = mp_obj_new_str(ev.label, strlen(ev.label));
    return mp_obj_new_tuple(3, out);
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_poll_for_result_obj, mp_seedsigner_lvgl_poll_for_result);

static mp_obj_t mp_seedsigner_lvgl_clear_result_queue(void) {
    s_result_head = 0;
    s_result_tail = 0;
    s_result_count = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_clear_result_queue_obj, mp_seedsigner_lvgl_clear_result_queue);

// --- Runtime + i18n -------------------------------------------------------
// Unified cross-platform surface: the shared Python app calls
// seedsigner_lvgl_screens.init() / .load_locale() / .unload_locale() identically
// on Pi Zero and ESP32 — no platform branching. The hardware-specific work (and,
// here, the SD-card pack provider + LVGL-port locking) lives behind these in
// display_manager.cpp; the Pi Zero binding implements the same names over its
// own backend.

// Full board-default bring-up (I2C, display, touch, LVGL port, display profile).
// Idempotent: hardware is already initialized at C boot, so this is a cheap
// re-entry that keeps the Pi-parity API (where init() does the real work).
static mp_obj_t mp_seedsigner_lvgl_init(void) {
    init();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_init_obj, mp_seedsigner_lvgl_init);

// set_screensaver_timeout(ms) -> None. Hands the idle timeout to the native
// overlay manager (0 disables the screensaver). The shared Python runner calls
// this once at startup with Controller's configured activation_ms; the overlay
// manager's dispatcher (started in init()) then owns the screensaver entirely.
// dm_* wrapper takes the LVGL-port lock — see display_manager.cpp.
static mp_obj_t mp_seedsigner_lvgl_set_screensaver_timeout(mp_obj_t ms_obj) {
    mp_int_t ms = mp_obj_get_int(ms_obj);
    if (ms < 0) {
        ms = 0;
    }
    dm_set_screensaver_timeout((uint32_t)ms);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(seedsigner_lvgl_set_screensaver_timeout_obj, mp_seedsigner_lvgl_set_screensaver_timeout);

// locale_pack_files(locale) -> JSON string array of the pack files this locale
// needs, e.g. '["th.ttf","runs.bin"]' (or '[]' for a baked-floor locale). The
// MicroPython side reads each of these off the SD card and passes the bytes to
// load_locale(). See ss_locale_pack_files() in locale_loader.h.
static mp_obj_t mp_seedsigner_lvgl_locale_pack_files(mp_obj_t locale_obj) {
    const char *locale = mp_obj_str_get_str(locale_obj);
    const char *json = ss_locale_pack_files(locale);
    return mp_obj_new_str(json, strlen(json));
}
static MP_DEFINE_CONST_FUN_OBJ_1(seedsigner_lvgl_locale_pack_files_obj, mp_seedsigner_lvgl_locale_pack_files);

// Pack provider backed by a Python dict {filename(str): bytes}. ESP-IDF's FAT
// stack can't be linked alongside MicroPython's own FAT VFS, so the SD card is
// read in Python (machine.SDCard) and the bytes are staged in this dict; the
// loader pulls each file it needs through here. The bytes objects are kept alive
// by the dict for the duration of the load, so returning their buffer is safe.
typedef struct {
    mp_obj_t packs;  // dict {str: bytes}
} mp_pack_ctx_t;

static bool mp_pack_provider(const char *locale, const char *file,
                             const uint8_t **bytes, size_t *len, void *user) {
    (void)locale;
    mp_pack_ctx_t *ctx = (mp_pack_ctx_t *)user;
    mp_map_t *map = mp_obj_dict_get_map(ctx->packs);
    mp_obj_t key = mp_obj_new_str(file, strlen(file));
    mp_map_elem_t *elem = mp_map_lookup(map, key, MP_MAP_LOOKUP);
    if (elem == NULL) {
        return false;  // pack staged dict is missing this file
    }
    mp_buffer_info_t bufinfo;
    if (!mp_get_buffer(elem->value, &bufinfo, MP_BUFFER_READ) || bufinfo.len == 0) {
        return false;  // value isn't a bytes-like buffer (don't raise under the LVGL lock)
    }
    *bytes = (const uint8_t *)bufinfo.buf;
    *len = bufinfo.len;
    return true;
}

// load_locale(locale, packs) -> bool. `packs` is a dict {filename: bytes} the
// caller pre-read from the SD card (the filenames come from locale_pack_files()).
// Returns True on full success, False if a needed pack is missing/unreadable
// (loader falls back to the baked Western floor).
static mp_obj_t mp_seedsigner_lvgl_load_locale(mp_obj_t locale_obj, mp_obj_t packs_obj) {
    const char *locale = mp_obj_str_get_str(locale_obj);
    if (!mp_obj_is_type(packs_obj, &mp_type_dict)) {
        mp_raise_TypeError(MP_ERROR_TEXT("load_locale expects (locale, packs_dict)"));
    }
    mp_pack_ctx_t ctx = { .packs = packs_obj };
    bool ok = dm_load_locale(locale, mp_pack_provider, &ctx);
    return mp_obj_new_bool(ok);
}
static MP_DEFINE_CONST_FUN_OBJ_2(seedsigner_lvgl_load_locale_obj, mp_seedsigner_lvgl_load_locale);

// Clear loaded locale packs and restore the baked Western floor.
static mp_obj_t mp_seedsigner_lvgl_unload_locale(void) {
    dm_unload_locale();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_unload_locale_obj, mp_seedsigner_lvgl_unload_locale);

// --- Instrumentation ------------------------------------------------------
// mem_stats() / get_memory_stats() -> dict. Reports the small internal LVGL
// builtin pool (CONFIG_LV_MEM_SIZE_KILOBYTES — 128 KB on P4-43, 64 KB on S3)
// alongside the ESP-IDF PSRAM and internal heaps. Glyph bitmaps and complex-
// script A8 masks are already routed to PSRAM, so what taxes the internal pool
// is the per-(font,px) cache index nodes, the live widget tree, and stb's
// rasterization scratch — see docs/font-memory-plan.md (Task D). `lvgl_max_used`
// is the high-water number to watch after each newly-ported screen and across
// locale switches; `lvgl_free_biggest` / `lvgl_frag_pct` flag fragmentation
// (which can crash with free bytes remaining). The two `*_min_free` fields are
// each heap's lowest-ever free size (high-water of use). Read over serial from
// the deployed app; keep logging permanent in debug builds so each screen self-
// reports a regression.
//
// The actual LVGL + esp_heap_caps queries live in dm_mem_stats() (display_
// manager.cpp), which already includes lvgl.h / esp_heap_caps.h and takes the
// LVGL-port lock. Keeping them there — behind a plain-C struct — avoids pulling
// those headers into this file, whose includes must resolve during MicroPython's
// QSTR scan (it sees only the dirs listed in bindings/micropython.cmake, not the
// transitive ESP-IDF component include paths).
static mp_obj_t mp_seedsigner_lvgl_mem_stats(void) {
    dm_mem_stats_t s;
    dm_mem_stats(&s);

    mp_obj_t d = mp_obj_new_dict(16);

    // LVGL builtin pool (internal DRAM): live occupancy + high-water + fragmentation.
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_lvgl_total), mp_obj_new_int_from_uint(s.lvgl_total));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_lvgl_free), mp_obj_new_int_from_uint(s.lvgl_free));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_lvgl_free_biggest), mp_obj_new_int_from_uint(s.lvgl_free_biggest));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_lvgl_max_used), mp_obj_new_int_from_uint(s.lvgl_max_used));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_lvgl_used_pct), mp_obj_new_int(s.lvgl_used_pct));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_lvgl_frag_pct), mp_obj_new_int(s.lvgl_frag_pct));

    // ESP-IDF heaps: free-now + minimum-ever-free (high-water) for PSRAM and internal.
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_spiram_free), mp_obj_new_int_from_uint(s.spiram_free));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_spiram_min_free), mp_obj_new_int_from_uint(s.spiram_min_free));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_internal_free), mp_obj_new_int_from_uint(s.internal_free));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_internal_min_free), mp_obj_new_int_from_uint(s.internal_min_free));

    // rb-cache PSRAM routing (Approach A): is the glyph/draw cache index living in
    // PSRAM, and is the route healthy? rb_psram_fallback should stay 0; with routing
    // on, lvgl_max_used should no longer climb under CJK and spiram absorbs the load.
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rb_psram_enabled), mp_obj_new_int_from_uint(s.rb_psram_enabled));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rb_psram_alloc), mp_obj_new_int_from_uint(s.rb_psram_alloc_total));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rb_psram_free), mp_obj_new_int_from_uint(s.rb_psram_free_total));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rb_psram_live_nodes), mp_obj_new_int_from_uint(s.rb_psram_live_nodes));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rb_psram_live_bytes), mp_obj_new_int_from_uint(s.rb_psram_live_bytes));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rb_psram_fallback), mp_obj_new_int_from_uint(s.rb_psram_fallback_total));

    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_mem_stats_obj, mp_seedsigner_lvgl_mem_stats);

// set_cache_psram(enabled) — runtime A/B toggle for Approach A rb-cache PSRAM
// routing. Flip off early in a measurement script to reproduce the original
// in-pool overflow as a control; on (default) for the fix. See dm_set_cache_psram().
static mp_obj_t mp_seedsigner_lvgl_set_cache_psram(mp_obj_t enabled_obj) {
    dm_set_cache_psram(mp_obj_is_true(enabled_obj));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(seedsigner_lvgl_set_cache_psram_obj, mp_seedsigner_lvgl_set_cache_psram);

static const mp_rom_map_elem_t seedsigner_lvgl_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_seedsigner_lvgl_screens) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&seedsigner_lvgl_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_screensaver_timeout), MP_ROM_PTR(&seedsigner_lvgl_set_screensaver_timeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_locale_pack_files), MP_ROM_PTR(&seedsigner_lvgl_locale_pack_files_obj) },
    { MP_ROM_QSTR(MP_QSTR_load_locale), MP_ROM_PTR(&seedsigner_lvgl_load_locale_obj) },
    { MP_ROM_QSTR(MP_QSTR_unload_locale), MP_ROM_PTR(&seedsigner_lvgl_unload_locale_obj) },
    { MP_ROM_QSTR(MP_QSTR_button_list_screen), MP_ROM_PTR(&seedsigner_lvgl_button_list_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_large_icon_status_screen), MP_ROM_PTR(&seedsigner_lvgl_large_icon_status_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_seed_add_passphrase_screen), MP_ROM_PTR(&seedsigner_lvgl_seed_add_passphrase_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_main_menu_screen), MP_ROM_PTR(&seedsigner_lvgl_main_menu_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_screensaver_screen), MP_ROM_PTR(&seedsigner_lvgl_screensaver_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll_for_result), MP_ROM_PTR(&seedsigner_lvgl_poll_for_result_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear_result_queue), MP_ROM_PTR(&seedsigner_lvgl_clear_result_queue_obj) },
    { MP_ROM_QSTR(MP_QSTR_mem_stats), MP_ROM_PTR(&seedsigner_lvgl_mem_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_memory_stats), MP_ROM_PTR(&seedsigner_lvgl_mem_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_cache_psram), MP_ROM_PTR(&seedsigner_lvgl_set_cache_psram_obj) },
};
static MP_DEFINE_CONST_DICT(seedsigner_lvgl_module_globals, seedsigner_lvgl_module_globals_table);

const mp_obj_module_t seedsigner_lvgl_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&seedsigner_lvgl_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_seedsigner_lvgl_screens, seedsigner_lvgl_user_cmodule);
