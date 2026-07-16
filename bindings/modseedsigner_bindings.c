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
    SEEDSIGNER_EVENT_QR_BRIGHTNESS,
    SEEDSIGNER_EVENT_QR_DENSITY,
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

// Override the weak default in components.cpp: a text-entry screen (e.g.
// seed_add_passphrase_screen) calls this on confirm with the entered text.
// Route it through the same queue so one poll loop sees both the confirmed
// text and a top-nav back-button press.
void seedsigner_lvgl_on_text_entered(const char *text) {
    seedsigner_result_enqueue(SEEDSIGNER_EVENT_TEXT_ENTERED, 0, text);
}

// Override the weak default in components.cpp: qr_display_screen calls this on exit
// with its final brightness (31..255). Route it through the same queue as a
// 'qr_brightness' event (brightness carried in the index field) so one poll loop
// sees the QR screen's exit and its brightness, letting the host persist
// SETTING__QR_BRIGHTNESS.
void seedsigner_lvgl_on_qr_brightness(uint8_t brightness) {
    seedsigner_result_enqueue(SEEDSIGNER_EVENT_QR_BRIGHTNESS, brightness, NULL);
}

// Override the weak default in qr_display_screen.cpp: the animated-QR density slider
// reports its selected pixels-per-module (3..6) here whenever the user changes it.
// Route it through the same queue as a 'qr_density' event (px/module carried in the
// index field, the same slot brightness uses) so one poll loop sees the QR screen's
// exit, its final brightness, AND each density change. The binding just marshals the
// int through: the host maps (vertical_resolution, px_per_module) -> max_fragment_len
// via its own density table (seedsigner/tools/qr_density_worksheet.py), rebuilds the
// UR encoder, and restarts the fountain (re-pushing frames via qr_display_set_frame).
// The C side never needs the table.
//
// NB: the screens library adds the weak seedsigner_lvgl_on_qr_density() decl + fire
// site as part of the QR density UI redesign; until that submodule bump lands this
// strong override is simply never called (harmless dead code). Signature is locked by
// the cross-repo spec: uint8_t px/module in {3,4,5,6}.
void seedsigner_lvgl_on_qr_density(uint8_t px_per_module) {
    seedsigner_result_enqueue(SEEDSIGNER_EVENT_QR_DENSITY, px_per_module, NULL);
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

// Shared cfg->JSON->run_screen path for the dict-config screens. The cfg arg is
// OPTIONAL: lvgl_screen_runner.py calls a native screen fn with () when its attrs
// are None, which would fault a strict 1-arg binding ("takes 1 positional argument
// but 0 given"). A 0-arg call (or an explicit None) means "no config" -> an empty
// JSON object; the screen-side C++ then fills its per-key defaults. A supplied arg
// must still be a dict (the localized title + labels the app passes in). Callers
// expose this via FUN_OBJ_VAR_BETWEEN(0, 1).
static mp_obj_t run_cfg_screen(display_manager_ui_callback_t fn, const char *name,
                               size_t n_args, const mp_obj_t *args) {
    vstr_t json;
    vstr_init(&json, 256);
    if (n_args >= 1 && args[0] != mp_const_none) {
        if (!mp_obj_is_type(args[0], &mp_type_dict)) {
            vstr_clear(&json);
            mp_raise_msg_varg(&mp_type_TypeError, MP_ERROR_TEXT("%s expects a dict"), name);
        }
        vstr_add_json_from_obj(&json, args[0]);
    } else {
        vstr_add_str(&json, "{}");
    }
    const char *err = run_screen(fn, (void *)json.buf);
    vstr_clear(&json);
    if (err) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("%s"), err);
    }
    return mp_const_none;
}

static mp_obj_t mp_seedsigner_lvgl_main_menu_screen(size_t n_args, const mp_obj_t *args) {
    // Display values are ALWAYS supplied by the caller in normal use: the app does the
    // gettext translation -- falling back to the English msgid -- and passes the result
    // in (localized top_nav.title + 4 button_list labels), exactly like button_list_screen.
    // The C side keeps internal defaults only as a per-key safety net (and for the 0-arg
    // runner path; see run_cfg_screen).
    return run_cfg_screen(main_menu_screen, "main_menu_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_main_menu_screen_obj, 0, 1, mp_seedsigner_lvgl_main_menu_screen);

// The app's opening splash (version, partner band, entrance animation). The
// Controller runs this at the start of its startup loop; the firmware already
// showed the centered boot logo (boot_logo_only) at C boot, so the app typically
// passes logo_already_shown=true and the animation continues from that position —
// a seamless boot->splash handoff. Reports completion via the poll queue.
static mp_obj_t mp_seedsigner_lvgl_opening_splash_screen(size_t n_args, const mp_obj_t *args) {
    // Canonical name after the split-seedsigner-screens reorg (was splash_screen); the
    // shared app calls it by this name (see seedsigner controller.py/view.py).
    return run_cfg_screen(opening_splash_screen, "opening_splash_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_opening_splash_screen_obj, 0, 1, mp_seedsigner_lvgl_opening_splash_screen);

static mp_obj_t mp_seedsigner_lvgl_screensaver_screen(void) {
    const char *err = run_screen(screensaver_screen, NULL);
    if (err) {
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s"), err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_screensaver_screen_obj, mp_seedsigner_lvgl_screensaver_screen);

static mp_obj_t mp_seedsigner_lvgl_button_list_screen(size_t n_args, const mp_obj_t *args) {
    // Pass JSON through mostly unchanged and let screen-side C++ validate.
    return run_cfg_screen(button_list_screen, "button_list_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_button_list_screen_obj, 0, 1, mp_seedsigner_lvgl_button_list_screen);

static mp_obj_t mp_seedsigner_lvgl_large_icon_status_screen(size_t n_args, const mp_obj_t *args) {
    // Pass JSON through mostly unchanged and let screen-side C++ validate.
    return run_cfg_screen(large_icon_status_screen, "large_icon_status_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_large_icon_status_screen_obj, 0, 1, mp_seedsigner_lvgl_large_icon_status_screen);

static mp_obj_t mp_seedsigner_lvgl_seed_add_passphrase_screen(size_t n_args, const mp_obj_t *args) {
    // Pass JSON through mostly unchanged and let screen-side C++ validate.
    return run_cfg_screen(seed_add_passphrase_screen, "seed_add_passphrase_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_seed_add_passphrase_screen_obj, 0, 1, mp_seedsigner_lvgl_seed_add_passphrase_screen);

// --- Keyboard/entry + QR-display screens (batched native-screen bindings) ------
// All three screens use the same dict-config shape as button_list_screen: the
// Python runner passes a cfg dict, run_cfg_screen forwards it as JSON, and the
// screen-side C++ validates + fills per-key defaults. Text results come back on
// the shared poll queue (confirm -> on_text_entered; top-nav back -> on_button_selected).

static mp_obj_t mp_seedsigner_lvgl_keyboard_screen(size_t n_args, const mp_obj_t *args) {
    // Generic keyboard. The Python caller supplies the charset/layout in the cfg,
    // so this one binding covers dice-roll, coin-flip, BIP85, index-number and
    // derivation-path entry -- each is just a different keyboard_screen cfg.
    return run_cfg_screen(keyboard_screen, "keyboard_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_keyboard_screen_obj, 0, 1, mp_seedsigner_lvgl_keyboard_screen);

static mp_obj_t mp_seedsigner_lvgl_seed_mnemonic_entry_screen(size_t n_args, const mp_obj_t *args) {
    // BIP39 word entry with the live word-match panel; confirms via on_text_entered.
    return run_cfg_screen(seed_mnemonic_entry_screen, "seed_mnemonic_entry_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_seed_mnemonic_entry_screen_obj, 0, 1, mp_seedsigner_lvgl_seed_mnemonic_entry_screen);

static mp_obj_t mp_seedsigner_lvgl_seed_finalize_screen(size_t n_args, const mp_obj_t *args) {
    // Fingerprint readout + bottom-pinned button list. cfg requires "fingerprint";
    // the screen-side C++ validates + supplies title/button_list defaults. Standard
    // polled screen: button selection comes back via on_button_selected.
    return run_cfg_screen(seed_finalize_screen, "seed_finalize_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_seed_finalize_screen_obj, 0, 1, mp_seedsigner_lvgl_seed_finalize_screen);

// loading_spinner_screen(cfg={"text": "..."}) -> None. The animated "processing" spinner
// shown while the host runs a long, blocking task (PSBT parse/verify, seed gen).
//
// BEHAVIOR IS DIFFERENT from every other screen binding above — read before use:
//
//   * FIRE-AND-FORGET, NOT POLLED. Every other screen is a builder the Python
//     runner then polls (run_lvgl_screen loops on poll_for_result until a terminal
//     event). loading_screen takes NO input and produces NO result, so it must NOT
//     be driven through that polling loop — the loop would spin forever. Python
//     calls this once, the call builds the widget tree and returns immediately, and
//     control goes straight back to the caller to start its long task.
//
//   * SELF-ANIMATED, NO HOST THREAD. run_screen builds the spinner and installs an
//     lv_timer that rotates the comet. That timer runs on the esp_lvgl_port task —
//     the same FreeRTOS task that pumps lv_timer_handler and renders every screen —
//     so the comet keeps spinning with zero host involvement while the MicroPython
//     thread is busy inside its blocking task. This REPLACES Python's
//     LoadingScreenThread (screen.py): on the PIL backend one thread can't both work
//     and repaint, so a background thread repaints and a stop event ends it; on the
//     LVGL backend the display task already repaints, so there is no Python thread
//     and no stop signal to manage.
//
//   * "STOP" == LOAD THE NEXT SCREEN. There is deliberately no stop()/hide() API.
//     When the task finishes, the host simply runs the next screen; the native
//     load_screen_and_cleanup_previous() deletes this screen, whose LV_EVENT_DELETE
//     handler tears down the spin timer. No explicit teardown, no join.
//
//   * CAVEAT: the comet advances only while the LVGL port task gets CPU. A task that
//     never yields the FreeRTOS scheduler (a tight, lock-free C loop) can starve it;
//     the self-driven timer is wall-clock clamped, so under load it visibly SLOWS
//     rather than jumping. Ordinary Python-level long tasks yield and animate fine.
static mp_obj_t mp_seedsigner_lvgl_loading_spinner_screen(size_t n_args, const mp_obj_t *args) {
    // Canonical name after the reorg (was loading_screen); the app calls
    // _lv.loading_spinner_screen (see seedsigner gui/lvgl_screen_runner.py).
    return run_cfg_screen(loading_spinner_screen, "loading_spinner_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_loading_spinner_screen_obj, 0, 1, mp_seedsigner_lvgl_loading_spinner_screen);

static mp_obj_t mp_seedsigner_lvgl_qr_display_screen(size_t n_args, const mp_obj_t *args) {
    // Static or animated QR. A static QR is fully described by the cfg; an animated
    // QR is host-driven -- after this call Python pushes each frame via
    // qr_display_set_frame() (gated on qr_display_is_tip_active()). Exit + final
    // brightness arrive on the poll queue (on_button_selected / on_qr_brightness).
    return run_cfg_screen(qr_display_screen, "qr_display_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_qr_display_screen_obj, 0, 1, mp_seedsigner_lvgl_qr_display_screen);

// qr_display_set_frame(data: bytes-like) -> None. Push the next animated-QR frame
// into the live qr_display_screen (re-encodes + repaints in place, reusing the
// cfg's qr_mode). `data` is raw bytes (may be binary, e.g. a CompactSeedQR payload).
// Safe no-op when no QR screen is active. See the animation contract in seedsigner.h.
static mp_obj_t mp_seedsigner_lvgl_qr_display_set_frame(mp_obj_t data_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
    // Route through the display_manager wrapper so the re-encode + lv_obj_invalidate
    // run under the LVGL-port lock (this binding is on the MicroPython task, concurrent
    // with the render task; a bare call races it and the frame never repaints).
    dm_qr_display_set_frame(bufinfo.buf, bufinfo.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(seedsigner_lvgl_qr_display_set_frame_obj, mp_seedsigner_lvgl_qr_display_set_frame);

// qr_display_is_tip_active() -> bool. True while the brightness tip panel is shown;
// the host frame driver HOLDS (doesn't call qr_display_set_frame) while true, then
// restarts the sequence when it clears. False when no QR screen is active.
static mp_obj_t mp_seedsigner_lvgl_qr_display_is_tip_active(void) {
    return mp_obj_new_bool(qr_display_is_tip_active());
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_qr_display_is_tip_active_obj, mp_seedsigner_lvgl_qr_display_is_tip_active);

// --- PSBT transaction-review screens (batched native-screen bindings) ----------
// LVGL ports of the SeedSigner PSBT review flow. All four use the same dict-config
// shape as button_list_screen: the Python runner passes a cfg dict, run_cfg_screen
// forwards it as JSON, and the screen-side C++ validates + fills per-key defaults.
// Each is a standard polled screen with a bottom-pinned button list, so button
// selection comes back on the shared poll queue (on_button_selected), exactly like
// seed_finalize_screen. The host owns all i18n (localized title + labels) and all
// amount formatting (btc_amount is a pure renderer -- the two platforms can never
// disagree on how an amount rounds). Per-screen cfg contracts are documented at each
// screen's own translation unit (screens/<name>_screen.cpp after the split reorg).

static mp_obj_t mp_seedsigner_lvgl_psbt_overview_screen(size_t n_args, const mp_obj_t *args) {
    // "Review Transaction": the transaction-flow pictogram (every input -> a shared
    // center bar -> recipients/self-transfers/change/OP_RETURN/fee) with a continuous
    // orange pulse, an optional BtcAmount headline, and a bottom "Review details" button.
    return run_cfg_screen(psbt_overview_screen, "psbt_overview_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_psbt_overview_screen_obj, 0, 1, mp_seedsigner_lvgl_psbt_overview_screen);

static mp_obj_t mp_seedsigner_lvgl_psbt_address_details_screen(size_t n_args, const mp_obj_t *args) {
    // One recipient's amount over its full, wrapped destination address, vertically
    // centered. cfg requires an "address" string (screen-side raises -> ValueError here).
    return run_cfg_screen(psbt_address_details_screen, "psbt_address_details_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_psbt_address_details_screen_obj, 0, 1, mp_seedsigner_lvgl_psbt_address_details_screen);

static mp_obj_t mp_seedsigner_lvgl_psbt_change_details_screen(size_t n_args, const mp_obj_t *args) {
    // The change / self-receive output: amount, a "change address #N" label, the
    // single-line address, and an optional "Address verified!" confirmation. cfg
    // requires an "address" string (screen-side raises -> ValueError here).
    return run_cfg_screen(psbt_change_details_screen, "psbt_change_details_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_psbt_change_details_screen_obj, 0, 1, mp_seedsigner_lvgl_psbt_change_details_screen);

static mp_obj_t mp_seedsigner_lvgl_psbt_math_screen(size_t n_args, const mp_obj_t *args) {
    // The fee "math": input - recipients - fee = change, right-aligned monospace with
    // btc-mode satoshi-zone dimming and an orange change unit. The host passes each
    // amount as an already-formatted number string plus the denomination flag.
    return run_cfg_screen(psbt_math_screen, "psbt_math_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_psbt_math_screen_obj, 0, 1, mp_seedsigner_lvgl_psbt_math_screen);

// --- Remaining custom-body screens (batched native-screen bindings) ------------
// LVGL ports of the last of SeedSigner's custom-body screens (multisig descriptor,
// sign-message confirm, address verification, SeedQR transcribe overview, SettingsQR
// import, calc-final-word, address-explorer list). All use the same dict-config shape
// as button_list_screen: the Python runner passes a cfg dict, run_cfg_screen forwards
// it as JSON, and the screen-side C++ validates + fills per-key defaults. Each is a
// standard polled screen with a bottom-pinned button list, so button selection comes
// back on the shared poll queue (on_button_selected), exactly like seed_finalize_screen.
// The host owns all i18n (localized title + labels) and all string formatting. Per-screen
// cfg contracts are documented in each screen's own translation unit
// (screens/<name>_screen.cpp) and its seedsigner.h declaration.

static mp_obj_t mp_seedsigner_lvgl_multisig_wallet_descriptor_screen(size_t n_args, const mp_obj_t *args) {
    // Multisig wallet-descriptor review: the policy (m-of-n), the signing-key xpubs,
    // and the space-joined cosigner fingerprints, over a bottom-pinned button list.
    return run_cfg_screen(multisig_wallet_descriptor_screen, "multisig_wallet_descriptor_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_multisig_wallet_descriptor_screen_obj, 0, 1, mp_seedsigner_lvgl_multisig_wallet_descriptor_screen);

static mp_obj_t mp_seedsigner_lvgl_seed_address_verification_screen(size_t n_args, const mp_obj_t *args) {
    // Brute-force address verification: the target address + its network, with an
    // in-place "Checking address N..." progress line the host updates live via
    // seed_address_verification_set_progress() while its background worker scans
    // derivation indexes. Standard polled bottom list (host owns the worker + match).
    return run_cfg_screen(seed_address_verification_screen, "seed_address_verification_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_seed_address_verification_screen_obj, 0, 1, mp_seedsigner_lvgl_seed_address_verification_screen);

// seed_address_verification_set_progress(text: str) -> None. Push the host's already-
// localized "Checking address N" text into the live seed_address_verification_screen's
// progress line (the library holds no strings). Fire-and-forget, NOT polled; safe no-op
// when no such screen is active -- the same host-driven live-push contract as
// qr_display_set_frame(). See seedsigner.h.
static mp_obj_t mp_seedsigner_lvgl_seed_address_verification_set_progress(mp_obj_t text_obj) {
    seed_address_verification_set_progress(mp_obj_str_get_str(text_obj));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(seedsigner_lvgl_seed_address_verification_set_progress_obj, mp_seedsigner_lvgl_seed_address_verification_set_progress);

static mp_obj_t mp_seedsigner_lvgl_seed_sign_message_confirm_message_screen(size_t n_args, const mp_obj_t *args) {
    // Sign-message flow: confirm the message text to be signed. cfg carries "message".
    return run_cfg_screen(seed_sign_message_confirm_message_screen, "seed_sign_message_confirm_message_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_seed_sign_message_confirm_message_screen_obj, 0, 1, mp_seedsigner_lvgl_seed_sign_message_confirm_message_screen);

static mp_obj_t mp_seedsigner_lvgl_seed_sign_message_confirm_address_screen(size_t n_args, const mp_obj_t *args) {
    // Sign-message flow: confirm the address (+ its derivation path) the message is
    // signed for, over a bottom-pinned button list.
    return run_cfg_screen(seed_sign_message_confirm_address_screen, "seed_sign_message_confirm_address_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_seed_sign_message_confirm_address_screen_obj, 0, 1, mp_seedsigner_lvgl_seed_sign_message_confirm_address_screen);

static mp_obj_t mp_seedsigner_lvgl_seed_transcribe_whole_qr_screen(size_t n_args, const mp_obj_t *args) {
    // The "whole QR" overview step of the SeedQR hand-transcription flow: direct-draws
    // the full SeedQR/CompactSeedQR grid (python-qrcode mask parity) with a pulsing
    // orange WarningEdges border. cfg carries qr_data / qr_mode / data_encoding / border.
    return run_cfg_screen(seed_transcribe_whole_qr_screen, "seed_transcribe_whole_qr_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_seed_transcribe_whole_qr_screen_obj, 0, 1, mp_seedsigner_lvgl_seed_transcribe_whole_qr_screen);

static mp_obj_t mp_seedsigner_lvgl_settings_qr_confirmation_screen(size_t n_args, const mp_obj_t *args) {
    // SettingsQR import confirmation: the imported config_name + a status_message.
    return run_cfg_screen(settings_qr_confirmation_screen, "settings_qr_confirmation_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_settings_qr_confirmation_screen_obj, 0, 1, mp_seedsigner_lvgl_settings_qr_confirmation_screen);

static mp_obj_t mp_seedsigner_lvgl_tools_calc_final_word_screen(size_t n_args, const mp_obj_t *args) {
    // Tools > Calc final word: the "final word math" breakdown -- the user's entered
    // entropy bits, the checksum bits (orange), and the merged final word over three
    // centered monospace bit rows.
    return run_cfg_screen(tools_calc_final_word_screen, "tools_calc_final_word_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_tools_calc_final_word_screen_obj, 0, 1, mp_seedsigner_lvgl_tools_calc_final_word_screen);

static mp_obj_t mp_seedsigner_lvgl_tools_calc_final_word_done_screen(size_t n_args, const mp_obj_t *args) {
    // Tools > Calc final word: the done screen -- the computed final word, the resulting
    // fingerprint, and the mnemonic word length, over a bottom-pinned button list.
    return run_cfg_screen(tools_calc_final_word_done_screen, "tools_calc_final_word_done_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_tools_calc_final_word_done_screen_obj, 0, 1, mp_seedsigner_lvgl_tools_calc_final_word_done_screen);

static mp_obj_t mp_seedsigner_lvgl_tools_address_explorer_address_list_screen(size_t n_args, const mp_obj_t *args) {
    // Address Explorer address list: a bottom-pinned, fixed-width (monospace) button
    // list of derived addresses, each "{index}:{head}...{tail}" (the focused row reveals
    // its full address); a trailing "Next N" button pages forward. cfg carries addresses
    // / start_index / initial_selected_index.
    return run_cfg_screen(tools_address_explorer_address_list_screen, "tools_address_explorer_address_list_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_tools_address_explorer_address_list_screen_obj, 0, 1, mp_seedsigner_lvgl_tools_address_explorer_address_list_screen);

// --- Seed view-detail screens (batched native-screen bindings) -----------------
// Implemented earlier (screens PR #56 seed words / xpub details / review passphrase,
// PR #60 zoomed transcribe) but not yet bound. Same dict-config shape as
// button_list_screen; standard polled screens returning on_button_selected. The app
// calls each by its canonical name (see seedsigner views/seed_views.py).

static mp_obj_t mp_seedsigner_lvgl_seed_words_screen(size_t n_args, const mp_obj_t *args) {
    // The mnemonic word list (parity with Python SeedWordsScreen): a paged grid of the
    // seed's BIP39 words with a bottom-pinned button list.
    return run_cfg_screen(seed_words_screen, "seed_words_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_seed_words_screen_obj, 0, 1, mp_seedsigner_lvgl_seed_words_screen);

static mp_obj_t mp_seedsigner_lvgl_seed_export_xpub_details_screen(size_t n_args, const mp_obj_t *args) {
    // Xpub export details (parity with Python SeedExportXpubDetailsScreen): the
    // fingerprint, derivation path, and xpub over icon_text_line rows, bottom button list.
    return run_cfg_screen(seed_export_xpub_details_screen, "seed_export_xpub_details_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_seed_export_xpub_details_screen_obj, 0, 1, mp_seedsigner_lvgl_seed_export_xpub_details_screen);

static mp_obj_t mp_seedsigner_lvgl_seed_review_passphrase_screen(size_t n_args, const mp_obj_t *args) {
    // Review passphrase (parity with Python SeedReviewPassphraseScreen): the entered
    // passphrase + resulting fingerprint, over a bottom-pinned button list.
    return run_cfg_screen(seed_review_passphrase_screen, "seed_review_passphrase_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_seed_review_passphrase_screen_obj, 0, 1, mp_seedsigner_lvgl_seed_review_passphrase_screen);

static mp_obj_t mp_seedsigner_lvgl_seed_transcribe_zoomed_qr_screen(size_t n_args, const mp_obj_t *args) {
    // Zoomed, pannable SeedQR transcription view (parity with Python
    // SeedTranscribeSeedQRZoomedInScreen): renders the QR oversized and steps one A-F/1-6
    // zone per joystick press / touch swipe (pan handled screen-side); exits via click / X
    // -> on_button_selected. cfg: qr_data / qr_mode / num_modules / initial_zone_x/y / exit_text.
    return run_cfg_screen(seed_transcribe_zoomed_qr_screen, "seed_transcribe_zoomed_qr_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_seed_transcribe_zoomed_qr_screen_obj, 0, 1, mp_seedsigner_lvgl_seed_transcribe_zoomed_qr_screen);

// --- Passthrough screens to reach full app parity (screens @ 057b6c6) -----------
// Each is a standard dict-cfg screen: the Python runner passes a localized cfg dict,
// run_cfg_screen forwards it as JSON, and the screen-side C++ parses + validates it.
// No bespoke marshaling — the cfg contract lives entirely in the Python app. Standard
// polled screens returning on_button_selected (or nav back). Named by the canonical
// screen fn (see seedsigner views/*). Grouped here to close the last binding gap.

static mp_obj_t mp_seedsigner_lvgl_power_options_screen(size_t n_args, const mp_obj_t *args) {
    // 2/4-tile large-icon grid (shares main_menu geometry). cfg: button_list (label +
    // icon per item), top_nav.title. Returns tile index; back -> RET_BACK_BUTTON.
    return run_cfg_screen(power_options_screen, "power_options_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_power_options_screen_obj, 0, 1, mp_seedsigner_lvgl_power_options_screen);

static mp_obj_t mp_seedsigner_lvgl_seed_address_verification_success_screen(size_t n_args, const mp_obj_t *args) {
    // LargeIconStatus subclass (SUCCESS icon + OK). cfg: status_headline, address,
    // address_type_text, index_text, button_list, top_nav.title.
    return run_cfg_screen(seed_address_verification_success_screen, "seed_address_verification_success_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_seed_address_verification_success_screen_obj, 0, 1, mp_seedsigner_lvgl_seed_address_verification_success_screen);

static mp_obj_t mp_seedsigner_lvgl_seed_transcribe_seedqr_format_screen(size_t n_args, const mp_obj_t *args) {
    // Standard/Compact SeedQR chooser, bottom-pinned. cfg: button_list, top_nav.title,
    // standard_label, standard_text, compact_label, compact_text.
    return run_cfg_screen(seed_transcribe_seedqr_format_screen, "seed_transcribe_seedqr_format_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_seed_transcribe_seedqr_format_screen_obj, 0, 1, mp_seedsigner_lvgl_seed_transcribe_seedqr_format_screen);

static mp_obj_t mp_seedsigner_lvgl_tools_address_explorer_address_type_screen(size_t n_args, const mp_obj_t *args) {
    // Receive/Change chooser. cfg: button_list, top_nav.title, and either
    // (fingerprint, fingerprint_label, derivation_text, derivation_label) or
    // (wallet_descriptor_text, wallet_descriptor_label).
    return run_cfg_screen(tools_address_explorer_address_type_screen, "tools_address_explorer_address_type_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_tools_address_explorer_address_type_screen_obj, 0, 1, mp_seedsigner_lvgl_tools_address_explorer_address_type_screen);

static mp_obj_t mp_seedsigner_lvgl_psbt_op_return_screen(size_t n_args, const mp_obj_t *args) {
    // PSBT OP_RETURN review. cfg: text OR (hex + hex_label), button_list, is_bottom_list.
    return run_cfg_screen(psbt_op_return_screen, "psbt_op_return_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_psbt_op_return_screen_obj, 0, 1, mp_seedsigner_lvgl_psbt_op_return_screen);

static mp_obj_t mp_seedsigner_lvgl_reset_screen(size_t n_args, const mp_obj_t *args) {
    // Info screen shown while the device restarts; no nav buttons. cfg: text.
    return run_cfg_screen(reset_screen, "reset_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_reset_screen_obj, 0, 1, mp_seedsigner_lvgl_reset_screen);

static mp_obj_t mp_seedsigner_lvgl_power_off_not_required_screen(size_t n_args, const mp_obj_t *args) {
    // Info + back button ("powering off isn't required on this hardware"). cfg: text.
    return run_cfg_screen(power_off_not_required_screen, "power_off_not_required_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_power_off_not_required_screen_obj, 0, 1, mp_seedsigner_lvgl_power_off_not_required_screen);

static mp_obj_t mp_seedsigner_lvgl_donate_screen(size_t n_args, const mp_obj_t *args) {
    // Donation info: paragraph + accent URL line. cfg: text, url.
    return run_cfg_screen(donate_screen, "donate_screen", n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_donate_screen_obj, 0, 1, mp_seedsigner_lvgl_donate_screen);

static mp_obj_t mp_seedsigner_lvgl_poll_for_result(void) {
    if (s_result_count == 0) {
        return mp_const_none;
    }

    seedsigner_result_event_t ev = s_result_queue[s_result_head];
    s_result_head = (s_result_head + 1) % SEEDSIGNER_RESULT_QUEUE_CAP;
    s_result_count--;

    qstr kind;
    switch (ev.kind) {
        case SEEDSIGNER_EVENT_TEXT_ENTERED:  kind = MP_QSTR_text_entered; break;
        case SEEDSIGNER_EVENT_QR_BRIGHTNESS: kind = MP_QSTR_qr_brightness; break;
        case SEEDSIGNER_EVENT_QR_DENSITY:    kind = MP_QSTR_qr_density; break;
        default:                             kind = MP_QSTR_button_selected; break;
    }

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

// --- Locale picker + runtime SD-card pack discovery -----------------------
// The language-selection screen and the "copy a pack onto the SD card, no
// firmware rebuild" flow. The C loader can't open the SD directly (the ESP-IDF
// FAT stack can't be linked alongside MicroPython's own FAT VFS), so the shared
// Python app lists the packs partition, reads each manifest.json + endonym blob
// via machine.SDCard, and drives these. See docs/language-selection-integration-
// todo.md and the screens repo's docs/knowledge/locale-picker-and-endonym-images.md.

// list_available_locales() -> JSON string. Every locale the firmware can render
// as a pack — compiled-in fonts UNION runtime-registered SD packs — for the
// active display profile, in supported_locales_json() shape ({"profile":...,
// "locales":[{"locale","source_family","chain","fonts":[...]}...]}). The app
// parses this and decorates each locale with its English display name + native
// endonym + the live-text-vs-image decision (baked-floor glyph coverage of the
// endonym), then builds the locale_picker_screen cfg. Baked-floor Latin locales
// (English, ...) are NOT listed — they need no font pack; the app adds them from
// its own translation catalog. See dm_supported_locales_json().
static mp_obj_t mp_seedsigner_lvgl_list_available_locales(void) {
    const char *json = dm_supported_locales_json();
    return mp_obj_new_str(json, strlen(json));
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_list_available_locales_obj, mp_seedsigner_lvgl_list_available_locales);

// register_pack_manifest(manifest) -> bool. `manifest` is the bytes/str of a
// language pack's own manifest.json (read off the SD card in Python). Registers
// the pack as a runtime locale so load_locale()/locale_pack_files()/
// list_available_locales() then work for a not-compiled-in code with no firmware
// rebuild. FAILS CLOSED: returns False on malformed JSON or a missing required
// field, so a corrupt/half-copied pack is simply skipped (never raises). See
// ss_register_pack_manifest() via dm_register_pack_manifest().
static mp_obj_t mp_seedsigner_lvgl_register_pack_manifest(mp_obj_t manifest_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(manifest_obj, &bufinfo, MP_BUFFER_READ);
    bool ok = dm_register_pack_manifest((const char *)bufinfo.buf, bufinfo.len);
    return mp_obj_new_bool(ok);
}
static MP_DEFINE_CONST_FUN_OBJ_1(seedsigner_lvgl_register_pack_manifest_obj, mp_seedsigner_lvgl_register_pack_manifest);

// clear_pack_manifests() -> None. Drop every runtime-registered pack (before an
// SD rescan). See ss_clear_pack_manifests() via dm_clear_pack_manifests().
static mp_obj_t mp_seedsigner_lvgl_clear_pack_manifests(void) {
    dm_clear_pack_manifests();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_clear_pack_manifests_obj, mp_seedsigner_lvgl_clear_pack_manifests);

// Endonym-image provider backed by a Python dict keyed "<locale>/<file>" -> bytes.
// The picker lists EVERY onboard language's name in its own script on ONE screen,
// so unlike load_locale (one active locale) it fetches many locales' images at
// once — all named "endonym_<h>.bin". Keying by filename alone (as mp_pack_provider
// does) would collide, so endonym blobs are staged under a composite "<locale>/<file>"
// key and this provider rebuilds that key from the (locale, file) the picker asks
// for. The picker copies each blob during the (synchronous) screen build, so the
// dict need only outlive the locale_picker_screen() call — it is passed as an
// argument, keeping it GC-alive for that duration. Reuses mp_pack_ctx_t.
static bool mp_endonym_provider(const char *locale, const char *file,
                                const uint8_t **bytes, size_t *len, void *user) {
    mp_pack_ctx_t *ctx = (mp_pack_ctx_t *)user;
    char key[96];
    int klen = snprintf(key, sizeof(key), "%s/%s", locale, file);
    if (klen <= 0 || (size_t)klen >= sizeof(key)) {
        return false;  // unexpectedly long key -> no image; the row keeps its live text
    }
    mp_map_t *map = mp_obj_dict_get_map(ctx->packs);
    mp_map_elem_t *elem = mp_map_lookup(map, mp_obj_new_str(key, klen), MP_MAP_LOOKUP);
    if (elem == NULL) {
        return false;  // this locale's endonym image wasn't staged
    }
    mp_buffer_info_t bufinfo;
    if (!mp_get_buffer(elem->value, &bufinfo, MP_BUFFER_READ) || bufinfo.len == 0) {
        return false;  // value isn't a bytes-like buffer (don't raise under the LVGL lock)
    }
    *bytes = (const uint8_t *)bufinfo.buf;
    *len = bufinfo.len;
    return true;
}

// locale_picker_screen(cfg=None, endonym_images=None) -> None. The language-
// selection screen: lists every onboard language's name in its own native script
// on one screen. Latin-script names render as live text; non-Latin names are
// pre-rendered A8 endonym images this binding serves through the picker's image
// provider.
//
//   cfg            — screen config dict ({top_nav, active_locale, rows:[...]}),
//                    same dict->JSON path as button_list_screen. A row carrying an
//                    "image" filename is an image row.
//   endonym_images — optional dict {"<locale>/<file>": bytes} holding each image
//                    row's pre-rendered endonym blob, e.g.
//                    {"hi/endonym_480.bin": b"SSA8..."}. The picker copies what it
//                    keeps during the synchronous build, so the dict need only live
//                    for this call; passing it as an argument keeps it GC-alive.
//                    Omit for a Latin-only picker (no image rows).
//
// Selection comes back on the shared poll queue as a button_selected event whose
// index is the row position — the host maps index -> the locale it placed there.
static mp_obj_t mp_seedsigner_lvgl_settings_locale_picker_screen(size_t n_args, const mp_obj_t *args) {
    // Wire the endonym image provider to the staging dict (if any) BEFORE building
    // the screen, and tear it down after so the C side never keeps a dangling
    // pointer to this call's stack ctx / the Python dict.
    mp_pack_ctx_t endonym_ctx = { .packs = mp_const_none };
    bool provider_set = false;
    if (n_args >= 2 && args[1] != mp_const_none) {
        if (!mp_obj_is_type(args[1], &mp_type_dict)) {
            mp_raise_TypeError(MP_ERROR_TEXT("settings_locale_picker_screen endonym_images must be a dict"));
        }
        endonym_ctx.packs = args[1];
        dm_set_endonym_image_provider(mp_endonym_provider, &endonym_ctx);
        provider_set = true;
    }

    // Serialize cfg (arg 0) to JSON and run the screen under the LVGL lock. Mirrors
    // run_cfg_screen, kept inline so the provider is cleared on every exit path.
    vstr_t json;
    vstr_init(&json, 256);
    if (n_args >= 1 && args[0] != mp_const_none) {
        if (!mp_obj_is_type(args[0], &mp_type_dict)) {
            vstr_clear(&json);
            if (provider_set) dm_set_endonym_image_provider(NULL, NULL);
            mp_raise_msg(&mp_type_TypeError, MP_ERROR_TEXT("settings_locale_picker_screen expects a dict"));
        }
        vstr_add_json_from_obj(&json, args[0]);
    } else {
        vstr_add_str(&json, "{}");
    }
    // Canonical name after the reorg (was locale_picker_screen); the app calls it by
    // this name (see seedsigner views/view.py).
    const char *err = run_screen(settings_locale_picker_screen, (void *)json.buf);
    vstr_clear(&json);

    // The picker fetched + copied every endonym during the build above, so the
    // provider is never called again; clear it so its user pointer (our stack ctx)
    // does not outlive this frame.
    if (provider_set) dm_set_endonym_image_provider(NULL, NULL);

    if (err) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("%s"), err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(seedsigner_lvgl_settings_locale_picker_screen_obj, 0, 2, mp_seedsigner_lvgl_settings_locale_picker_screen);

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

// display_size() -> (width, height). The active display profile's pixel dimensions
// (e.g. (480, 480) on the P4 4.3", (320, 480) on a 3.5"). Available before any locale
// load. The animated-QR density lookup keys on the REAL panel height, but the app's
// LvglRenderer.canvas_height otherwise falls back to a static 240 default on this path;
// this getter lets the host resolve the true panel so the density table picks the right
// row. The C++ active_profile() lives behind dm_display_size() (display_manager.cpp) so
// gui_constants.h (C++) stays out of this file's QSTR-scan include set -- same split as
// dm_mem_stats.
static mp_obj_t mp_seedsigner_lvgl_display_size(void) {
    int w = 0, h = 0;
    dm_display_size(&w, &h);
    mp_obj_t out[2];
    out[0] = mp_obj_new_int(w);
    out[1] = mp_obj_new_int(h);
    return mp_obj_new_tuple(2, out);
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_display_size_obj, mp_seedsigner_lvgl_display_size);

// --- Toast overlay (transient banner) -------------------------------------
// Platform-symmetric with the Pi Zero .so; see docs/toast-binding-contract.md.
// A toast is NOT a screen: it has no cfg->JSON->run_screen path and no result. It's
// a fire-and-forget push to the native overlay manager, which composites it on the
// LVGL top layer over whatever screen is live and owns auto-dismiss / input-dismiss /
// one-at-a-time / screensaver coexistence. The library is policy-free — the app
// resolves severity -> (glyph, colors) and passes finished values.

// Look up an optional key in a cfg dict; MP_OBJ_NULL if absent (vs mp_const_none,
// which the app never sends for these but we treat as "use default" too).
static mp_obj_t cfg_dict_get(mp_obj_t dict, qstr key) {
    mp_map_t *map = mp_obj_dict_get_map(dict);
    mp_map_elem_t *e = mp_map_lookup(map, MP_OBJ_NEW_QSTR(key), MP_MAP_LOOKUP);
    return e ? e->value : MP_OBJ_NULL;
}

// show_toast(cfg) -> None. Parse the cfg dict directly into flat args (NOT via
// run_cfg_screen/JSON — the native API takes a struct) and stage it thread-safely.
// cfg: label_text (str, required), icon (str PUA glyph -> icon_glyph, optional),
// outline_color / font_color (int 0xRRGGBB, default white), duration_ms (int,
// default 3000; 0 = stay until dismissed/replaced).
static mp_obj_t mp_seedsigner_lvgl_show_toast(mp_obj_t cfg_obj) {
    if (!mp_obj_is_type(cfg_obj, &mp_type_dict)) {
        mp_raise_msg(&mp_type_TypeError, MP_ERROR_TEXT("show_toast expects a dict"));
    }

    mp_obj_t label = cfg_dict_get(cfg_obj, MP_QSTR_label_text);
    const char *label_text = (label && label != mp_const_none) ? mp_obj_str_get_str(label) : "";

    mp_obj_t icon = cfg_dict_get(cfg_obj, MP_QSTR_icon);
    const char *icon_glyph = (icon && icon != mp_const_none) ? mp_obj_str_get_str(icon) : NULL;

    mp_obj_t oc = cfg_dict_get(cfg_obj, MP_QSTR_outline_color);
    uint32_t outline_color = (oc && oc != mp_const_none) ? (uint32_t)mp_obj_get_int_truncated(oc) : 0xFFFFFF;

    mp_obj_t fc = cfg_dict_get(cfg_obj, MP_QSTR_font_color);
    uint32_t font_color = (fc && fc != mp_const_none) ? (uint32_t)mp_obj_get_int_truncated(fc) : 0xFFFFFF;

    mp_obj_t dur = cfg_dict_get(cfg_obj, MP_QSTR_duration_ms);
    uint32_t duration_ms = (dur && dur != mp_const_none) ? (uint32_t)mp_obj_get_int(dur) : 3000;

    dm_show_toast(label_text, icon_glyph, outline_color, font_color, duration_ms);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(seedsigner_lvgl_show_toast_obj, mp_seedsigner_lvgl_show_toast);

// dismiss_toast() -> None. Dismiss the current toast immediately (no-op if none).
// LVGL-thread only (dm_* wrapper takes the LVGL-port lock); routine toasts self-
// expire on duration_ms, so the app never calls this from a producer thread.
static mp_obj_t mp_seedsigner_lvgl_dismiss_toast(void) {
    dm_dismiss_toast();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_dismiss_toast_obj, mp_seedsigner_lvgl_dismiss_toast);

static const mp_rom_map_elem_t seedsigner_lvgl_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__seedsigner_lvgl_screens) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&seedsigner_lvgl_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_screensaver_timeout), MP_ROM_PTR(&seedsigner_lvgl_set_screensaver_timeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_locale_pack_files), MP_ROM_PTR(&seedsigner_lvgl_locale_pack_files_obj) },
    { MP_ROM_QSTR(MP_QSTR_load_locale), MP_ROM_PTR(&seedsigner_lvgl_load_locale_obj) },
    { MP_ROM_QSTR(MP_QSTR_unload_locale), MP_ROM_PTR(&seedsigner_lvgl_unload_locale_obj) },
    { MP_ROM_QSTR(MP_QSTR_list_available_locales), MP_ROM_PTR(&seedsigner_lvgl_list_available_locales_obj) },
    { MP_ROM_QSTR(MP_QSTR_register_pack_manifest), MP_ROM_PTR(&seedsigner_lvgl_register_pack_manifest_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear_pack_manifests), MP_ROM_PTR(&seedsigner_lvgl_clear_pack_manifests_obj) },
    { MP_ROM_QSTR(MP_QSTR_settings_locale_picker_screen), MP_ROM_PTR(&seedsigner_lvgl_settings_locale_picker_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_button_list_screen), MP_ROM_PTR(&seedsigner_lvgl_button_list_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_large_icon_status_screen), MP_ROM_PTR(&seedsigner_lvgl_large_icon_status_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_seed_add_passphrase_screen), MP_ROM_PTR(&seedsigner_lvgl_seed_add_passphrase_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_keyboard_screen), MP_ROM_PTR(&seedsigner_lvgl_keyboard_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_seed_mnemonic_entry_screen), MP_ROM_PTR(&seedsigner_lvgl_seed_mnemonic_entry_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_seed_finalize_screen), MP_ROM_PTR(&seedsigner_lvgl_seed_finalize_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_loading_spinner_screen), MP_ROM_PTR(&seedsigner_lvgl_loading_spinner_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_qr_display_screen), MP_ROM_PTR(&seedsigner_lvgl_qr_display_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_qr_display_set_frame), MP_ROM_PTR(&seedsigner_lvgl_qr_display_set_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_qr_display_is_tip_active), MP_ROM_PTR(&seedsigner_lvgl_qr_display_is_tip_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_psbt_overview_screen), MP_ROM_PTR(&seedsigner_lvgl_psbt_overview_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_psbt_address_details_screen), MP_ROM_PTR(&seedsigner_lvgl_psbt_address_details_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_psbt_change_details_screen), MP_ROM_PTR(&seedsigner_lvgl_psbt_change_details_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_psbt_math_screen), MP_ROM_PTR(&seedsigner_lvgl_psbt_math_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_multisig_wallet_descriptor_screen), MP_ROM_PTR(&seedsigner_lvgl_multisig_wallet_descriptor_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_seed_address_verification_screen), MP_ROM_PTR(&seedsigner_lvgl_seed_address_verification_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_seed_address_verification_set_progress), MP_ROM_PTR(&seedsigner_lvgl_seed_address_verification_set_progress_obj) },
    { MP_ROM_QSTR(MP_QSTR_seed_sign_message_confirm_message_screen), MP_ROM_PTR(&seedsigner_lvgl_seed_sign_message_confirm_message_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_seed_sign_message_confirm_address_screen), MP_ROM_PTR(&seedsigner_lvgl_seed_sign_message_confirm_address_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_seed_transcribe_whole_qr_screen), MP_ROM_PTR(&seedsigner_lvgl_seed_transcribe_whole_qr_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_settings_qr_confirmation_screen), MP_ROM_PTR(&seedsigner_lvgl_settings_qr_confirmation_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_tools_calc_final_word_screen), MP_ROM_PTR(&seedsigner_lvgl_tools_calc_final_word_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_tools_calc_final_word_done_screen), MP_ROM_PTR(&seedsigner_lvgl_tools_calc_final_word_done_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_tools_address_explorer_address_list_screen), MP_ROM_PTR(&seedsigner_lvgl_tools_address_explorer_address_list_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_seed_words_screen), MP_ROM_PTR(&seedsigner_lvgl_seed_words_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_seed_export_xpub_details_screen), MP_ROM_PTR(&seedsigner_lvgl_seed_export_xpub_details_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_seed_review_passphrase_screen), MP_ROM_PTR(&seedsigner_lvgl_seed_review_passphrase_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_seed_transcribe_zoomed_qr_screen), MP_ROM_PTR(&seedsigner_lvgl_seed_transcribe_zoomed_qr_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_power_options_screen), MP_ROM_PTR(&seedsigner_lvgl_power_options_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_seed_address_verification_success_screen), MP_ROM_PTR(&seedsigner_lvgl_seed_address_verification_success_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_seed_transcribe_seedqr_format_screen), MP_ROM_PTR(&seedsigner_lvgl_seed_transcribe_seedqr_format_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_tools_address_explorer_address_type_screen), MP_ROM_PTR(&seedsigner_lvgl_tools_address_explorer_address_type_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_psbt_op_return_screen), MP_ROM_PTR(&seedsigner_lvgl_psbt_op_return_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset_screen), MP_ROM_PTR(&seedsigner_lvgl_reset_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_power_off_not_required_screen), MP_ROM_PTR(&seedsigner_lvgl_power_off_not_required_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_donate_screen), MP_ROM_PTR(&seedsigner_lvgl_donate_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_show_toast), MP_ROM_PTR(&seedsigner_lvgl_show_toast_obj) },
    { MP_ROM_QSTR(MP_QSTR_dismiss_toast), MP_ROM_PTR(&seedsigner_lvgl_dismiss_toast_obj) },
    { MP_ROM_QSTR(MP_QSTR_main_menu_screen), MP_ROM_PTR(&seedsigner_lvgl_main_menu_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_opening_splash_screen), MP_ROM_PTR(&seedsigner_lvgl_opening_splash_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_screensaver_screen), MP_ROM_PTR(&seedsigner_lvgl_screensaver_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll_for_result), MP_ROM_PTR(&seedsigner_lvgl_poll_for_result_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear_result_queue), MP_ROM_PTR(&seedsigner_lvgl_clear_result_queue_obj) },
    { MP_ROM_QSTR(MP_QSTR_mem_stats), MP_ROM_PTR(&seedsigner_lvgl_mem_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_memory_stats), MP_ROM_PTR(&seedsigner_lvgl_mem_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_cache_psram), MP_ROM_PTR(&seedsigner_lvgl_set_cache_psram_obj) },
    { MP_ROM_QSTR(MP_QSTR_display_size), MP_ROM_PTR(&seedsigner_lvgl_display_size_obj) },
};
static MP_DEFINE_CONST_DICT(seedsigner_lvgl_module_globals, seedsigner_lvgl_module_globals_table);

const mp_obj_module_t seedsigner_lvgl_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&seedsigner_lvgl_module_globals,
};

// Registered as the PRIVATE name `_seedsigner_lvgl_screens`. The public import
// name `seedsigner_lvgl_screens` (what the shared app imports) is a thin frozen /
// /lib Python façade that wraps this C module and hides the ESP32 SD-card I/O
// behind the same dir-based locale API the Pi native module (seedsigner-raspi-lvgl)
// exposes — so the app calls the same names on both platforms. The façade reads
// pack bytes off the SD (machine.SDCard) and hands them to the byte-based C API
// here (load_locale/register_pack_manifest/locale_picker_screen); the C side can't
// open the SD directly (ESP-IDF fatfs vs MicroPython oofatfs link collision).
MP_REGISTER_MODULE(MP_QSTR__seedsigner_lvgl_screens, seedsigner_lvgl_user_cmodule);
