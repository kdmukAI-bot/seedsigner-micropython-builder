/*
 * camera_scanner — MicroPython binding over the builder's QR-scan composition.
 *
 * The consumer's SOLE API to the scan pipeline (contract:
 * docs/camera-pipeline-phase2-poll-contract.md). A thin skin over the plain-C
 * cam_scanner_* surface in the camera_scanner component (which owns the pipeline +
 * overlay + scan_coordinator). Module-level functions over a single session — there
 * is only ever one camera.
 *
 * Drain-then-read each consumer loop:
 *     camera_scanner.start()
 *     while not done:
 *         while (p := camera_scanner.poll_new()) is not None:   # drain NEW ring
 *             status, pct = decoder.add_data(p)
 *             camera_scanner.report(status, pct)                # -> overlay
 *         st = camera_scanner.read_status()                     # coalesced status
 *         if st.consecutive_misses >= THRESHOLD: warn(...)
 *     camera_scanner.report_complete()
 *     camera_scanner.stop()
 *
 * read_status() returns a STRUCTURED object (attrtuple), not a positional tuple, so
 * the reserved corners field can be added later without breaking call sites (§7
 * tier 2). This is an internal system-to-system contract, hence attribute access
 * (st.latest) rather than a human-facing dict.
 */
#include <stdint.h>
#include <string.h>

/* 480-decode build variant (SS_CAM_DECODE_480=1 -> usermod compile definition
 * in bindings/micropython.cmake). Deliberately NOT from a board header — this
 * TU stays QSTR-scan-clean. Exported as camera_scanner.DECODE_480. */
#ifndef BOARD_CAMERA_DECODE_480
#define BOARD_CAMERA_DECODE_480 0
#endif

#include "py/obj.h"
#include "py/objtuple.h"   // mp_obj_new_attrtuple
#include "py/runtime.h"

#include "camera_scanner.h"

// start(focus_assist=False) -> None. Raises OSError with a short reason on
// bring-up failure. focus_assist=True brings up the camera preview with an
// on-screen software focus meter (quirc skipped) instead of the QR scan overlay;
// in that mode poll_new()/read_status()/report() are inert. start() with no
// args is the normal scan session (unchanged for existing call sites).
static mp_obj_t mp_camera_scanner_start(size_t n_args, const mp_obj_t *pos_args,
                                        mp_map_t *kw_args) {
    enum { ARG_focus_assist };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_focus_assist, MP_ARG_BOOL, { .u_bool = false } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args),
                     allowed_args, args);

    const char *err = cam_scanner_start(args[ARG_focus_assist].u_bool);
    if (err) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("%s"), err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(camera_scanner_start_obj, 0, mp_camera_scanner_start);

// stop() -> None.
static mp_obj_t mp_camera_scanner_stop(void) {
    cam_scanner_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_scanner_stop_obj, mp_camera_scanner_stop);

// is_running() -> bool.
static mp_obj_t mp_camera_scanner_is_running(void) {
    return mp_obj_new_bool(cam_scanner_is_running());
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_scanner_is_running_obj, mp_camera_scanner_is_running);

// poll_new() -> bytes | None. Drains one NEW payload from the ring, copying it into
// a fresh bytes (the C buffer is valid only until the next poll). None when empty.
static mp_obj_t mp_camera_scanner_poll_new(void) {
    const uint8_t *payload = NULL;
    size_t len = 0;
    if (!cam_scanner_poll_new(&payload, &len)) {
        return mp_const_none;
    }
    return mp_obj_new_bytes(payload, len);
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_scanner_poll_new_obj, mp_camera_scanner_poll_new);

// poll_miss_frame() -> (bytes, meta) | None. DIAGNOSTIC: drains the latest sampled
// located-but-undecoded frame (only produced during a scan on a debug build). The
// bytes are a width*height grayscale crop; meta is an attrtuple describing why it
// missed (quirc_err) and how it looked (side_px / sharpness / luma). None when no
// new miss is available. The C buffer is valid only until the next call, so the
// bytes are copied out here at once.
static mp_obj_t mp_camera_scanner_poll_miss_frame(void) {
    const uint8_t *payload = NULL;
    size_t len = 0;
    cam_scanner_miss_meta_t m;
    if (!cam_scanner_poll_miss_frame(&payload, &len, &m)) {
        return mp_const_none;
    }
    static const qstr fields[] = {
        MP_QSTR_seq, MP_QSTR_timestamp_us, MP_QSTR_quirc_err,
        MP_QSTR_side_px, MP_QSTR_sharpness, MP_QSTR_luma_mean,
        MP_QSTR_width, MP_QSTR_height,
    };
    mp_obj_t meta_items[] = {
        mp_obj_new_int_from_uint(m.seq),
        mp_obj_new_int_from_ll(m.timestamp_us),
        MP_OBJ_NEW_SMALL_INT(m.quirc_err),
        mp_obj_new_float(m.side_px),
        mp_obj_new_float(m.sharpness),
        MP_OBJ_NEW_SMALL_INT(m.luma_mean),
        mp_obj_new_int_from_uint(m.width),
        mp_obj_new_int_from_uint(m.height),
    };
    mp_obj_t ret[] = {
        mp_obj_new_bytes(payload, len),
        mp_obj_new_attrtuple(fields, MP_ARRAY_SIZE(meta_items), meta_items),
    };
    return mp_obj_new_tuple(2, ret);
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_scanner_poll_miss_frame_obj, mp_camera_scanner_poll_miss_frame);

// read_status() -> attrtuple(latest, consecutive_misses, dropped_new, has_corners).
static mp_obj_t mp_camera_scanner_read_status(void) {
    cam_scanner_status_t st;
    cam_scanner_read_status(&st);

    static const qstr fields[] = {
        MP_QSTR_latest,
        MP_QSTR_consecutive_misses,
        MP_QSTR_dropped_new,
        MP_QSTR_has_corners,
    };
    mp_obj_t items[] = {
        MP_OBJ_NEW_SMALL_INT(st.latest),
        mp_obj_new_int_from_uint(st.consecutive_misses),
        mp_obj_new_int_from_uint(st.dropped_new),
        mp_obj_new_bool(st.has_corners),
    };
    return mp_obj_new_attrtuple(fields, MP_ARRAY_SIZE(items), items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_scanner_read_status_obj, mp_camera_scanner_read_status);

// report(status, percent) -> None. status is one of FRAME_*; percent 0..100.
static mp_obj_t mp_camera_scanner_report(mp_obj_t status_obj, mp_obj_t percent_obj) {
    cam_scanner_report(mp_obj_get_int(status_obj), mp_obj_get_int(percent_obj));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(camera_scanner_report_obj, mp_camera_scanner_report);

// report_complete() -> None.
static mp_obj_t mp_camera_scanner_report_complete(void) {
    cam_scanner_report_complete();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_scanner_report_complete_obj, mp_camera_scanner_report_complete);

// ── Instrumentation-run surface (docs/instrumentation-run-spec.md) ──────────
// set_instr(**knobs) stages a per-session config consumed by the NEXT start();
// the poll_* functions drain the telemetry the decode tasks produce so the
// sequencer can write the SD files. All knobs default to deployed behavior.

// Stats-block mirror sequencing (reset per set_instr so a fresh session's
// blocks always read as new).
static uint32_t s_instr_stats_seq;

// set_instr(num_decoders=0, sweep_cap=0, ladder=0, blend_gate=0, dedup=False,
//           debounce_ms=0, seed_pin=-999 (off), lock_freeze=False,
//           fixed_threshold=False, thorough=False, hash_gate=True,
//           instr_log=False, capture_nothing=False, burst=False) -> None
static mp_obj_t mp_camera_scanner_set_instr(size_t n_args, const mp_obj_t *pos_args,
                                            mp_map_t *kw_args) {
    enum {
        ARG_num_decoders, ARG_sweep_cap, ARG_ladder, ARG_blend_gate, ARG_dedup,
        ARG_debounce_ms, ARG_seed_pin, ARG_lock_freeze, ARG_fixed_threshold,
        ARG_thorough, ARG_hash_gate, ARG_instr_log, ARG_capture_nothing,
        ARG_burst,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_num_decoders, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_sweep_cap, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_ladder, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_blend_gate, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_dedup, MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_debounce_ms, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_seed_pin, MP_ARG_INT, { .u_int = -999 } },
        { MP_QSTR_lock_freeze, MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_fixed_threshold, MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_thorough, MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_hash_gate, MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_instr_log, MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_capture_nothing, MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_burst, MP_ARG_BOOL, { .u_bool = false } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args),
                     allowed_args, args);

    cam_scanner_instr_opts_t opts;
    cam_scanner_instr_defaults(&opts);
    opts.num_decoders = args[ARG_num_decoders].u_int;
    opts.sweep_cap = args[ARG_sweep_cap].u_int;
    opts.ladder_select = args[ARG_ladder].u_int;
    opts.blend_gate_permille = args[ARG_blend_gate].u_int;
    opts.gate_dedup = args[ARG_dedup].u_bool;
    opts.debounce_ms = args[ARG_debounce_ms].u_int;
    if (args[ARG_seed_pin].u_int != -999) {
        opts.seed_override = 1;
        opts.seed_offset = args[ARG_seed_pin].u_int;
    }
    opts.lock_freeze = args[ARG_lock_freeze].u_bool;
    opts.fixed_threshold = args[ARG_fixed_threshold].u_bool;
    opts.effort_thorough = args[ARG_thorough].u_bool;
    opts.hash_gate = args[ARG_hash_gate].u_bool;
    opts.instr_log = args[ARG_instr_log].u_bool;
    opts.capture_nothing = args[ARG_capture_nothing].u_bool;
    opts.burst = args[ARG_burst].u_bool;
    cam_scanner_set_instr(&opts);
    s_instr_stats_seq = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(camera_scanner_set_instr_obj, 0, mp_camera_scanner_set_instr);

// poll_instr_csv(which) -> bytes | None. which: 0 = decode CSV, 1 = gate CSV.
// Drains up to ~2 KB of complete rows per call; call until None.
static mp_obj_t mp_camera_scanner_poll_instr_csv(mp_obj_t which_obj) {
    static char buf[2048];
    size_t n = cam_scanner_instr_poll_csv(mp_obj_get_int(which_obj), buf, sizeof(buf));
    if (n == 0) {
        return mp_const_none;
    }
    return mp_obj_new_bytes((const uint8_t *)buf, n);
}
static MP_DEFINE_CONST_FUN_OBJ_1(camera_scanner_poll_instr_csv_obj, mp_camera_scanner_poll_instr_csv);

// poll_instr_stats() -> str | None. Latest 2 s stats block (run.log mirror);
// None until a block newer than the last one polled exists.
static mp_obj_t mp_camera_scanner_poll_instr_stats(void) {
    static char buf[560];
    if (!cam_scanner_instr_poll_stats(buf, sizeof(buf), &s_instr_stats_seq)) {
        return mp_const_none;
    }
    return mp_obj_new_str(buf, strlen(buf));
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_scanner_poll_instr_stats_obj, mp_camera_scanner_poll_instr_stats);

// poll_instr_capture() -> (bytes, meta) | None. Drains the oldest pending
// frame capture (grayscale crop). meta.cls: 0 miss, 1 blend, 2 decoded,
// 3 nothing, 4 burst.
static mp_obj_t mp_camera_scanner_poll_instr_capture(void) {
    const uint8_t *payload = NULL;
    size_t len = 0;
    cam_scanner_capture_meta_t m;
    if (!cam_scanner_instr_poll_capture(&payload, &len, &m)) {
        return mp_const_none;
    }
    static const qstr fields[] = {
        MP_QSTR_cls, MP_QSTR_seq, MP_QSTR_dispatch_seq, MP_QSTR_timestamp_us,
        MP_QSTR_width, MP_QSTR_height, MP_QSTR_decoder_id, MP_QSTR_outcome,
        MP_QSTR_blend_score, MP_QSTR_side_px, MP_QSTR_sharpness, MP_QSTR_luma,
    };
    mp_obj_t meta_items[] = {
        MP_OBJ_NEW_SMALL_INT(m.cls),
        mp_obj_new_int_from_uint(m.seq),
        mp_obj_new_int_from_uint(m.dispatch_seq),
        mp_obj_new_int_from_ll(m.ts_us),
        mp_obj_new_int_from_uint(m.width),
        mp_obj_new_int_from_uint(m.height),
        MP_OBJ_NEW_SMALL_INT(m.decoder_id),
        MP_OBJ_NEW_SMALL_INT(m.outcome),
        MP_OBJ_NEW_SMALL_INT(m.blend_score),
        mp_obj_new_float(m.side_px),
        mp_obj_new_float(m.sharpness),
        MP_OBJ_NEW_SMALL_INT(m.luma),
    };
    mp_obj_t ret[] = {
        mp_obj_new_bytes(payload, len),
        mp_obj_new_attrtuple(fields, MP_ARRAY_SIZE(meta_items), meta_items),
    };
    return mp_obj_new_tuple(2, ret);
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_scanner_poll_instr_capture_obj, mp_camera_scanner_poll_instr_capture);

// instr_counters() -> attrtuple. Producer-side drop audit + fragment counters.
static mp_obj_t mp_camera_scanner_instr_counters(void) {
    cam_scanner_instr_counters_t c;
    cam_scanner_instr_counters(&c);
    static const qstr fields[] = {
        MP_QSTR_decode_rows, MP_QSTR_decode_drops, MP_QSTR_gate_rows,
        MP_QSTR_gate_drops, MP_QSTR_captures, MP_QSTR_capture_drops,
        MP_QSTR_frag_arrived, MP_QSTR_frag_dispatched, MP_QSTR_frag_expired,
    };
    mp_obj_t items[] = {
        mp_obj_new_int_from_uint(c.decode_rows),
        mp_obj_new_int_from_uint(c.decode_drops),
        mp_obj_new_int_from_uint(c.gate_rows),
        mp_obj_new_int_from_uint(c.gate_drops),
        mp_obj_new_int_from_uint(c.captures),
        mp_obj_new_int_from_uint(c.capture_drops),
        mp_obj_new_int_from_uint(c.frag_arrived),
        mp_obj_new_int_from_uint(c.frag_dispatched),
        mp_obj_new_int_from_uint(c.frag_expired),
    };
    return mp_obj_new_attrtuple(fields, MP_ARRAY_SIZE(items), items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_scanner_instr_counters_obj, mp_camera_scanner_instr_counters);

// reset_lock() -- cold-start the adaptive-threshold lock on the live decoders,
// so the next decode runs a full acquisition sweep. Called between BBQR-bench
// completion trials to make each an independent cold-start sample. No-op when
// the sweep/instrumentation is compiled out; safe to call while scanning.
static mp_obj_t mp_camera_scanner_reset_lock(void) {
    cam_scanner_reset_lock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_scanner_reset_lock_obj, mp_camera_scanner_reset_lock);

static const mp_rom_map_elem_t camera_scanner_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_camera_scanner) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&camera_scanner_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&camera_scanner_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_running), MP_ROM_PTR(&camera_scanner_is_running_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll_new), MP_ROM_PTR(&camera_scanner_poll_new_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll_miss_frame), MP_ROM_PTR(&camera_scanner_poll_miss_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_status), MP_ROM_PTR(&camera_scanner_read_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_report), MP_ROM_PTR(&camera_scanner_report_obj) },
    { MP_ROM_QSTR(MP_QSTR_report_complete), MP_ROM_PTR(&camera_scanner_report_complete_obj) },
    // Instrumentation-run surface (per-slot knobs + telemetry drains).
    { MP_ROM_QSTR(MP_QSTR_set_instr), MP_ROM_PTR(&camera_scanner_set_instr_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll_instr_csv), MP_ROM_PTR(&camera_scanner_poll_instr_csv_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll_instr_stats), MP_ROM_PTR(&camera_scanner_poll_instr_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll_instr_capture), MP_ROM_PTR(&camera_scanner_poll_instr_capture_obj) },
    { MP_ROM_QSTR(MP_QSTR_instr_counters), MP_ROM_PTR(&camera_scanner_instr_counters_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset_lock), MP_ROM_PTR(&camera_scanner_reset_lock_obj) },
    // Frame-status vocabulary (mirrors scan_coordinator / Python DecodeQRStatus).
    { MP_ROM_QSTR(MP_QSTR_FRAME_NONE), MP_ROM_INT(CAM_SCAN_FRAME_NONE) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_NEW), MP_ROM_INT(CAM_SCAN_FRAME_NEW) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_REPEAT), MP_ROM_INT(CAM_SCAN_FRAME_REPEAT) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_MISS), MP_ROM_INT(CAM_SCAN_FRAME_MISS) },
    // Build-variant marker: 1 when this firmware decodes at 480x480 with the
    // 3->2 decimated preview (SS_CAM_DECODE_480 build), else 0. Lets frozen
    // Python (e.g. the instrumentation sequencer BUILD_TAG) label the variant.
    { MP_ROM_QSTR(MP_QSTR_DECODE_480), MP_ROM_INT(BOARD_CAMERA_DECODE_480) },
};
static MP_DEFINE_CONST_DICT(camera_scanner_module_globals, camera_scanner_module_globals_table);

const mp_obj_module_t camera_scanner_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&camera_scanner_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_camera_scanner, camera_scanner_user_cmodule);
