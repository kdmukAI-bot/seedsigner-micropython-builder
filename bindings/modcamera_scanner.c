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

#include "py/obj.h"
#include "py/objtuple.h"   // mp_obj_new_attrtuple
#include "py/runtime.h"

#include "camera_scanner.h"

// start() -> None. Raises OSError with a short reason on bring-up failure.
static mp_obj_t mp_camera_scanner_start(void) {
    const char *err = cam_scanner_start();
    if (err) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("%s"), err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_scanner_start_obj, mp_camera_scanner_start);

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

static const mp_rom_map_elem_t camera_scanner_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_camera_scanner) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&camera_scanner_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&camera_scanner_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_running), MP_ROM_PTR(&camera_scanner_is_running_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll_new), MP_ROM_PTR(&camera_scanner_poll_new_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_status), MP_ROM_PTR(&camera_scanner_read_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_report), MP_ROM_PTR(&camera_scanner_report_obj) },
    { MP_ROM_QSTR(MP_QSTR_report_complete), MP_ROM_PTR(&camera_scanner_report_complete_obj) },
    // Frame-status vocabulary (mirrors scan_coordinator / Python DecodeQRStatus).
    { MP_ROM_QSTR(MP_QSTR_FRAME_NONE), MP_ROM_INT(CAM_SCAN_FRAME_NONE) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_NEW), MP_ROM_INT(CAM_SCAN_FRAME_NEW) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_REPEAT), MP_ROM_INT(CAM_SCAN_FRAME_REPEAT) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_MISS), MP_ROM_INT(CAM_SCAN_FRAME_MISS) },
};
static MP_DEFINE_CONST_DICT(camera_scanner_module_globals, camera_scanner_module_globals_table);

const mp_obj_module_t camera_scanner_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&camera_scanner_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_camera_scanner, camera_scanner_user_cmodule);
