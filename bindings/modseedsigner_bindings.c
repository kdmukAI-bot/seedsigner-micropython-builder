#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "display_manager.h"
#include "seedsigner.h"

#define SEEDSIGNER_RESULT_QUEUE_CAP 16
#define SEEDSIGNER_RESULT_LABEL_MAX 96

typedef struct {
    uint32_t index;
    char label[SEEDSIGNER_RESULT_LABEL_MAX];
} seedsigner_result_event_t;

static seedsigner_result_event_t s_result_queue[SEEDSIGNER_RESULT_QUEUE_CAP];
static uint32_t s_result_head = 0;
static uint32_t s_result_tail = 0;
static uint32_t s_result_count = 0;

void seedsigner_lvgl_on_button_selected(uint32_t index, const char *label) {
    seedsigner_result_event_t ev = {
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

static void vstr_add_json_escaped(vstr_t *v, const char *src, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        char c = src[i];
        switch (c) {
            case '\\': vstr_add_str(v, "\\\\"); break;
            case '"': vstr_add_str(v, "\\\""); break;
            case '\n': vstr_add_str(v, "\\n"); break;
            case '\r': vstr_add_str(v, "\\r"); break;
            case '\t': vstr_add_str(v, "\\t"); break;
            default: vstr_add_char(v, c); break;
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

static mp_obj_t mp_seedsigner_lvgl_demo_screen(void) {
    const char *err = run_screen(demo_screen, NULL);
    if (err) {
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s"), err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_demo_screen_obj, mp_seedsigner_lvgl_demo_screen);

static mp_obj_t mp_seedsigner_lvgl_main_menu_screen(void) {
    const char *err = run_screen(main_menu_screen, NULL);
    if (err) {
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s"), err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_main_menu_screen_obj, mp_seedsigner_lvgl_main_menu_screen);

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

static mp_obj_t mp_seedsigner_lvgl_poll_for_result(void) {
    if (s_result_count == 0) {
        return mp_const_none;
    }

    seedsigner_result_event_t ev = s_result_queue[s_result_head];
    s_result_head = (s_result_head + 1) % SEEDSIGNER_RESULT_QUEUE_CAP;
    s_result_count--;

    mp_obj_t out[3];
    out[0] = MP_OBJ_NEW_QSTR(MP_QSTR_button_selected);
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

static const mp_rom_map_elem_t seedsigner_lvgl_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_seedsigner_lvgl) },
    { MP_ROM_QSTR(MP_QSTR_demo_screen), MP_ROM_PTR(&seedsigner_lvgl_demo_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_button_list_screen), MP_ROM_PTR(&seedsigner_lvgl_button_list_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_main_menu_screen), MP_ROM_PTR(&seedsigner_lvgl_main_menu_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_screensaver_screen), MP_ROM_PTR(&seedsigner_lvgl_screensaver_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll_for_result), MP_ROM_PTR(&seedsigner_lvgl_poll_for_result_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear_result_queue), MP_ROM_PTR(&seedsigner_lvgl_clear_result_queue_obj) },
};
static MP_DEFINE_CONST_DICT(seedsigner_lvgl_module_globals, seedsigner_lvgl_module_globals_table);

const mp_obj_module_t seedsigner_lvgl_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&seedsigner_lvgl_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_seedsigner_lvgl, seedsigner_lvgl_user_cmodule);
