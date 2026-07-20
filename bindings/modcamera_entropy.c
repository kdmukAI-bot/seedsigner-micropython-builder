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
 *     chain, frame, n = r                       # both mutable — see get_result()
 *     hasher = hashlib.sha256()                 # fed incrementally: `chain + frame`
 *     hasher.update(chain)                      # would build a third full-size copy
 *     hasher.update(frame)                      # of the frame just to hash it
 *     final = hasher.digest()                   # the entropy result
 *     camera_entropy.secure_zero(frame)         # scrub the copies once hashed
 *     camera_entropy.secure_zero(chain)
 *     camera_entropy.stop()                     # (or .resume() to reshoot)
 *
 * The chain digest EXCLUDES the latched final image; the host mixes it in via the
 * final SHA-256, matching the SeedSigner reference so the result is reproducible.
 */
#include <stdint.h>
#include <string.h>

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

// Optional positional string arg -> const char *, NULL when absent or None.
static const char *opt_str(size_t n_args, const mp_obj_t *args, size_t i) {
    if (i >= n_args || args[i] == mp_const_none) {
        return NULL;
    }
    return mp_obj_str_get_str(args[i]);
}

// set_labels(capturing_text, accept_label, preview_instructions, confirm_instructions) -> None.
// Supply the overlay's localized strings (the app's gettext) BEFORE start() so nothing is
// hardcoded in the overlay. Any arg may be None/omitted. Persists until changed. The shutter's
// camera icon is a symbol the overlay supplies itself.
//
// Args 1-2 are the TOUCH affordances (the CAPTURING transient + the CONFIRM accept button) and
// are what this board renders today. Args 3-4 are the HARDWARE-input bottom instruction lines
// ("< back | click a button"), rendered by the overlay only under INPUT_MODE_HARDWARE — inert
// on a touch panel. They are accepted and plumbed through regardless so the ONE cross-platform
// host loop can pass all four unconditionally (the Pi hardware-mode path already does), and so
// an ESP board with physical inputs needs no binding change. See
// docs/camera-entropy-contract-conformance-todo.md.
static mp_obj_t mp_camera_entropy_set_labels(size_t n_args, const mp_obj_t *args) {
    cam_entropy_set_labels(opt_str(n_args, args, 0),   // capturing_text
                           opt_str(n_args, args, 1),   // accept_label
                           opt_str(n_args, args, 2),   // preview_instructions (hardware mode)
                           opt_str(n_args, args, 3));  // confirm_instructions (hardware mode)
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(camera_entropy_set_labels_obj, 0, 4, mp_camera_entropy_set_labels);

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

// get_result() -> (chain: bytearray, frame: bytearray, frames_chained: int) | None.
// None until the latch completes after capture() (poll a few ms). `chain` is the
// 32-byte digest over seed + preview frames; `frame` is the latched RGB565 final
// image. The host computes the entropy as sha256(chain || frame).
//
// MUTABLE bytearrays rather than bytes, deliberately: both are seed material, and
// what the host receives are copies it owns. Immutable bytes could only ever be
// de-referenced, and neither VM zeroes memory it frees — so the plaintext frame
// would sit in freed blocks until something happened to reuse them. A bytearray can
// be scrubbed in place; see secure_zero() below, which the host calls once it has
// hashed them. The native latch these are copied from is scrubbed separately by
// cam_pipeline_entropy on teardown.
static mp_obj_t mp_camera_entropy_get_result(void) {
    const uint8_t *chain = NULL, *frame = NULL;
    size_t chain_len = 0, frame_len = 0;
    uint32_t n = 0;
    if (!cam_entropy_get_result(&chain, &chain_len, &frame, &frame_len, &n)) {
        return mp_const_none;
    }
    mp_obj_t out[3];
    out[0] = mp_obj_new_bytearray(chain_len, chain);
    out[1] = mp_obj_new_bytearray(frame_len, frame);
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

/* memset reached through a volatile function pointer. The compiler must load the
 * pointer and call through it, so it cannot prove the write is dead and drop it —
 * while still getting a real memset's speed, which matters when the frame is
 * megabytes. A hand-rolled `volatile uint8_t *` loop is equally DSE-proof but byte
 * at a time. */
static void *(*const volatile secure_memset)(void *, int, size_t) = memset;

// secure_zero(buf) -> None. Overwrite a writable buffer with zeros, in place.
//
// For the host to scrub the get_result() copies (or any other buffer holding seed
// material) once they have been hashed. Two properties matter here and neither is
// reachable from pure Python:
//   * IN PLACE. `buf[:] = b"\x00" * len(buf)` is allowed to reallocate, which would
//     free the original plaintext buffer un-scrubbed — precisely backwards. Writing
//     through the buffer protocol touches the caller's own memory and nothing else.
//   * NOT OPTIMIZED AWAY. A plain memset over memory never read again is a dead
//     store, and compilers do eliminate it. See secure_memset above.
//
// Raises TypeError on a read-only object (e.g. bytes), so a scrub that could not
// possibly land fails loudly instead of silently doing nothing.
//
// Deliberately a mechanical wipe and nothing else: no part of the seed derivation
// moves into C, so that math stays readable, auditable Python.
static mp_obj_t mp_camera_entropy_secure_zero(mp_obj_t buf_in) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_RW);
    secure_memset(bufinfo.buf, 0, bufinfo.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(camera_entropy_secure_zero_obj, mp_camera_entropy_secure_zero);

static const mp_rom_map_elem_t camera_entropy_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_camera_entropy) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&camera_entropy_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&camera_entropy_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_labels), MP_ROM_PTR(&camera_entropy_set_labels_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_running), MP_ROM_PTR(&camera_entropy_is_running_obj) },
    { MP_ROM_QSTR(MP_QSTR_frames_chained), MP_ROM_PTR(&camera_entropy_frames_chained_obj) },
    { MP_ROM_QSTR(MP_QSTR_capture), MP_ROM_PTR(&camera_entropy_capture_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_result), MP_ROM_PTR(&camera_entropy_get_result_obj) },
    { MP_ROM_QSTR(MP_QSTR_resume), MP_ROM_PTR(&camera_entropy_resume_obj) },
    { MP_ROM_QSTR(MP_QSTR_secure_zero), MP_ROM_PTR(&camera_entropy_secure_zero_obj) },
};
static MP_DEFINE_CONST_DICT(camera_entropy_module_globals, camera_entropy_module_globals_table);

const mp_obj_module_t camera_entropy_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&camera_entropy_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_camera_entropy, camera_entropy_user_cmodule);
