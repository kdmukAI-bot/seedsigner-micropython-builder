/*
 * camera_entropy — builder-side composition of the image-entropy capture flow,
 * exposed to MicroPython. Mirrors camera_scanner: a single-session singleton that
 * owns the camera preview pipeline, but attaches the cam_pipeline_entropy consumer
 * (SHA-256 chain over live frames) instead of a QR scan_coordinator. No overlay —
 * the live preview renders into the active LVGL screen; the host drives capture /
 * review / reshoot and builds its own on-screen UI.
 *
 * Flow (host / Python side):
 *     camera_entropy.start()                 # live preview, chaining begins
 *     while collecting:
 *         n = camera_entropy.frames_chained()  # drive a progress indicator
 *     camera_entropy.capture()               # freeze the on-screen frame + latch it
 *     while (r := camera_entropy.get_result()) is None:  # poll a few ms
 *         pass
 *     chain, frame, n = r
 *     final = hashlib.sha256(chain + frame).digest()      # the entropy result
 *     # ...use `final` as the seed entropy; or reshoot:
 *     camera_entropy.resume()
 *     camera_entropy.stop()
 *
 * The chain digest EXCLUDES the latched final image (the host mixes it in via the
 * final SHA-256), matching the SeedSigner Pi Zero implementation so the host can
 * finish the hash identically. Nothing is persisted; buffers are zeroized on stop.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stand up the preview pipeline + entropy consumer. `seed_hash` is an OPTIONAL
 * caller-supplied uniqueness seed (e.g. a hash of device uptime), NOT an entropy
 * source — pass NULL/0 for none; if given it must be 32 bytes. Idempotent.
 * Returns NULL on success, or a short static reason string on failure. */
const char *cam_entropy_start(const uint8_t *seed_hash, size_t seed_len);

/* Tear down consumer + pipeline. Idempotent. Zeroizes the chain digest + latch. */
void cam_entropy_stop(void);

/* True while a capture session is live (between start and stop). */
bool cam_entropy_is_running(void);

/* Number of preview frames mixed into the chain so far (live progress counter). */
uint32_t cam_entropy_frames_chained(void);

/* Arm capture: on its next iteration the consumer freezes the display on the
 * current frame, latches it as the final image, and freezes the chain digest at
 * its pre-final value. Idempotent while already captured. */
void cam_entropy_capture(void);

/* Retrieve the frozen result after capture(). Returns false until the latch
 * completes (poll a few ms) or if not currently captured. On success:
 *   chain          -> 32-byte chained digest over seed + all preview frames
 *   frame          -> latched RGB565 final-image bytes (frame_len bytes)
 *   frames_chained -> preview frames mixed into the chain
 * The chain/frame pointers stay valid until resume() or stop(). Any out-pointer
 * may be NULL. */
bool cam_entropy_get_result(const uint8_t **chain, size_t *chain_len,
                            const uint8_t **frame, size_t *frame_len,
                            uint32_t *frames_chained);

/* Cancel / reshoot: discard the latched final image, unfreeze the pipeline, and
 * resume chaining live frames (accumulated preview entropy is retained). */
void cam_entropy_resume(void);

#ifdef __cplusplus
}
#endif
