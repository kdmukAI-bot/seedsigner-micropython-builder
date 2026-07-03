/*
 * camera_entropy — MicroPython binding over the builder's image-entropy capture
 * composition. A thin skin over the plain-C cam_entropy_* surface in the
 * camera_entropy component (which owns the pipeline + cam_pipeline_entropy
 * consumer). Module-level functions over a single session — there is only ever
 * one camera. Companion to the camera_scanner module (QR scanning).
 *
 * Host loop (Python):
 *     camera_entropy.start()                    # or start(seed_hash=<32 bytes>)
 *     while collecting:
 *         progress(camera_entropy.frames_chained())
 *     camera_entropy.capture()
 *     while (r := camera_entropy.get_result()) is None:
 *         pass                                  # poll a few ms for the latch
 *     chain, frame, n = r
 *     final = hashlib.sha256(chain + frame).digest()   # the entropy result
 *     camera_entropy.stop()                     # (or .resume() to reshoot)
 *
 * The chain digest EXCLUDES the latched final image; the host mixes it in via the
 * final SHA-256, matching the SeedSigner reference so the result is reproducible.
 */
#include <stdint.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "camera_entropy.h"

// start(seed_hash=None) -> None. Optional 32-byte caller uniqueness seed (bytes),
// NOT an entropy source. Raises OSError with a short reason on bring-up failure.
static mp_obj_t mp_camera_entropy_start(size_t n_args, const mp_obj_t *args) {
    const uint8_t *seed = NULL;
    size_t seed_len = 0;
    if (n_args >= 1 && args[0] != mp_const_none) {
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(args[0], &bufinfo, MP_BUFFER_READ);
        seed = (const uint8_t *)bufinfo.buf;
        seed_len = bufinfo.len;
    }
    const char *err = cam_entropy_start(seed, seed_len);
    if (err) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("%s"), err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(camera_entropy_start_obj, 0, 1, mp_camera_entropy_start);

// stop() -> None.
static mp_obj_t mp_camera_entropy_stop(void) {
    cam_entropy_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_entropy_stop_obj, mp_camera_entropy_stop);

// is_running() -> bool.
static mp_obj_t mp_camera_entropy_is_running(void) {
    return mp_obj_new_bool(cam_entropy_is_running());
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_entropy_is_running_obj, mp_camera_entropy_is_running);

// frames_chained() -> int. Live count of preview frames mixed into the chain
// (drives a progress indicator while collecting).
static mp_obj_t mp_camera_entropy_frames_chained(void) {
    return mp_obj_new_int_from_uint(cam_entropy_frames_chained());
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_entropy_frames_chained_obj, mp_camera_entropy_frames_chained);

// capture() -> None. Arm capture: freeze the on-screen frame + latch it as final.
static mp_obj_t mp_camera_entropy_capture(void) {
    cam_entropy_capture();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_entropy_capture_obj, mp_camera_entropy_capture);

// get_result() -> (chain: bytes, frame: bytes, frames_chained: int) | None.
// None until the latch completes after capture() (poll a few ms). `chain` is the
// 32-byte digest over seed + preview frames; `frame` is the latched RGB565 final
// image. The host computes the entropy as sha256(chain + frame).
static mp_obj_t mp_camera_entropy_get_result(void) {
    const uint8_t *chain = NULL, *frame = NULL;
    size_t chain_len = 0, frame_len = 0;
    uint32_t n = 0;
    if (!cam_entropy_get_result(&chain, &chain_len, &frame, &frame_len, &n)) {
        return mp_const_none;
    }
    mp_obj_t out[3];
    out[0] = mp_obj_new_bytes(chain, chain_len);
    out[1] = mp_obj_new_bytes(frame, frame_len);
    out[2] = mp_obj_new_int_from_uint(n);
    return mp_obj_new_tuple(3, out);
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_entropy_get_result_obj, mp_camera_entropy_get_result);

// resume() -> None. Discard the latch, unfreeze, and resume chaining (reshoot).
static mp_obj_t mp_camera_entropy_resume(void) {
    cam_entropy_resume();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_entropy_resume_obj, mp_camera_entropy_resume);

static const mp_rom_map_elem_t camera_entropy_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_camera_entropy) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&camera_entropy_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&camera_entropy_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_running), MP_ROM_PTR(&camera_entropy_is_running_obj) },
    { MP_ROM_QSTR(MP_QSTR_frames_chained), MP_ROM_PTR(&camera_entropy_frames_chained_obj) },
    { MP_ROM_QSTR(MP_QSTR_capture), MP_ROM_PTR(&camera_entropy_capture_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_result), MP_ROM_PTR(&camera_entropy_get_result_obj) },
    { MP_ROM_QSTR(MP_QSTR_resume), MP_ROM_PTR(&camera_entropy_resume_obj) },
};
static MP_DEFINE_CONST_DICT(camera_entropy_module_globals, camera_entropy_module_globals_table);

const mp_obj_module_t camera_entropy_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&camera_entropy_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_camera_entropy, camera_entropy_user_cmodule);
