/**
 * camera_entropy — builder-side composition of the image-entropy capture flow
 * (see camera_entropy.h for the contract). Owns pipeline + entropy consumer as a
 * single-session singleton; the MicroPython bindings are a thin skin over the
 * cam_entropy_* C API below. Mirrors camera_scanner, minus the overlay/coordinator.
 */
#include "camera_entropy.h"

#include <string.h>

#include "board.h"
#include "board_config.h"

#include "esp_log.h"

static const char *TAG = "camera_entropy";

/* Partition mode (P4-35 ST7796): LVGL keeps running during the preview. Default
 * off for boards that use the LVGL-stopped dummy-draw path. */
#ifndef BOARD_CAMERA_PARTITION_MODE
#define BOARD_CAMERA_PARTITION_MODE 0
#endif

/* High-resolution entropy still, per board (P4 only): the latched final image is
 * a SQUARE at 2x the display square (960 on the P4-43, 640 on the P4-35), grabbed
 * by a second PPA pass, hashed full-resolution, and shown downsampled with pillar
 * bars. 0 = no still: latch the display-resolution preview square (S3 / no PPA).
 * See docs/_integration/entropy-highres-still-todo.md. */
#ifndef BOARD_ENTROPY_STILL_DIM
#define BOARD_ENTROPY_STILL_DIM 0
#endif

#if BOARD_HAS_CAMERA

/* The engine headers lack an extern "C" guard. Force C linkage HERE, before any
 * board header (board_pipeline.h) pulls esp_cam_pipeline.h in under C++ linkage —
 * once that happens its include guard would defeat a later wrap and leave
 * cam_pipeline_create() C++-mangled (link error). Same ordering as camera_scanner. */
extern "C" {
#include "esp_cam_pipeline.h"
#include "cam_pipeline_entropy.h"
}

#include "board_pipeline.h"
#include "board_i2c.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "camera_entropy_overlay.h"  /* seedsigner-lvgl-screens overlay (LVGL) */
#include "image_entropy.h"           /* seedsigner-lvgl-screens display processing (BUILDER-1) */
#include "esp_heap_caps.h"

/* Single live session: one camera, one entropy consumer, tied to start()/stop(). */
static cam_pipeline_handle_t          s_pipeline = NULL;
static cam_pipeline_entropy_handle_t  s_entropy  = NULL;
static camera_entropy_overlay_t      *s_overlay  = NULL;
static lv_obj_t                      *s_screen   = NULL;
static bool                           s_running  = false;
static bool                           s_await_confirm = false;  /* capture()→CONFIRM latch */
static volatile uint32_t              s_frames   = 0;  /* live progress counter */
static uint32_t                       s_square   = 0;  /* display/preview square side (px) */
static uint32_t                       s_still_w  = 0;  /* latched-still width  (px) */
static uint32_t                       s_still_h  = 0;  /* latched-still height (px) */
static uint8_t                        s_seed[32];      /* copy of optional seed */

/* Overlay strings are HOST-PROVIDED + already localized (cam_entropy_set_labels, called
 * by the app's gettext BEFORE start). Nothing is hardcoded here; empty until set. The
 * shutter's camera icon is a symbol supplied by the overlay itself. */
static char s_capturing_text[80] = {0};  /* CAPTURING transient, e.g. "Capturing image..." */
static char s_accept_label[40]   = {0};  /* CONFIRM accept button, e.g. "Accept" */
/* Hardware-input bottom instruction lines. The overlay draws these only under
 * INPUT_MODE_HARDWARE, so they are inert on a touch panel — carried so the shared host
 * loop can pass all four labels unconditionally and a physical-input ESP board needs no
 * plumbing change. See docs/camera-entropy-contract-conformance-todo.md. */
static char s_preview_instructions[80] = {0};  /* PREVIEW, e.g. "< back  |  click a button" */
static char s_confirm_instructions[80] = {0};  /* CONFIRM, e.g. "< reshoot  |  accept >"    */

/* Flip the overlay phase under the LVGL lock (host thread calls capture/resume/etc). */
static void overlay_set_phase(camera_entropy_phase_t phase)
{
    if (s_overlay && lvgl_port_lock(0)) {
        camera_entropy_overlay_set_phase(s_overlay, phase);
        lvgl_port_unlock();
    }
}

/* ── Option A dedicated camera screen ───────────────────────────────────────────
 * The camera image renders onto its OWN opaque-black screen rather than over the
 * previously-active one, so the warm-up window (and the teardown→next-load gap)
 * shows black instead of the screen the user came from. This matters more here than
 * for the scanner: camera_entropy has NO overlay, so on a landscape panel its own
 * black background is also the only thing covering the side gutters during capture.
 * See docs/camera-pipeline-teardown-screen-reveal.md. ── */

/* Create the black cam screen and load it. *out_prev receives the screen that was
 * active before the swap; the caller deletes it on success (the destination View
 * rebuilds it fresh on return, per the View-layer contract) or restores it on a
 * start failure. Returns NULL on alloc failure. Caller must hold the LVGL lock. */
static lv_obj_t *cam_make_black_screen(lv_obj_t **out_prev)
{
    lv_obj_t *prev = lv_screen_active();
    if (out_prev) {
        *out_prev = prev;
    }
    lv_obj_t *scr = lv_obj_create(NULL);
    if (!scr) {
        return NULL;
    }
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(scr);
    return scr;
}

/* Undo the start-time screen swap when a later start step fails: revert the render
 * interval, restore the pre-camera screen, and drop our black one, so the caller
 * sees exactly the state it had before start() was attempted. */
static void cam_rollback_screen(lv_obj_t *prev_screen)
{
    if (lvgl_port_lock(0)) {
        board_set_render_interval_ms(0);
        if (prev_screen) {
            lv_screen_load(prev_screen);
        }
        if (s_screen) {
            lv_obj_delete(s_screen);
            s_screen = NULL;
        }
        lvgl_port_unlock();
    }
}

/* Fires once per chained preview frame on the consumer task — just record the
 * count for cam_entropy_frames_chained() (must return quickly). */
static void on_entropy_frame(uint32_t frames_chained, void *ctx)
{
    (void)ctx;
    s_frames = frames_chained;
}

static void copy_label(char *dst, size_t cap, const char *src)
{
    if (src && src[0]) {
        strncpy(dst, src, cap - 1);
        dst[cap - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

void cam_entropy_set_labels(const char *capturing_text, const char *accept_label,
                            const char *preview_instructions, const char *confirm_instructions)
{
    copy_label(s_capturing_text,       sizeof(s_capturing_text),       capturing_text);
    copy_label(s_accept_label,         sizeof(s_accept_label),         accept_label);
    copy_label(s_preview_instructions, sizeof(s_preview_instructions), preview_instructions);
    copy_label(s_confirm_instructions, sizeof(s_confirm_instructions), confirm_instructions);
}

const char *cam_entropy_start(const uint8_t *seed_hash, size_t seed_len)
{
    if (s_running) {
        return NULL;  /* idempotent */
    }
    if (seed_hash && seed_len != 0 && seed_len != 32) {
        return "seed_hash must be 32 bytes";
    }

    /* Build on our OWN opaque-black screen (Option A) rather than the active one, so
     * the warm-up window shows black instead of the screen the user came from (the
     * camera image renders as its child). Short render interval for a responsive
     * real-time preview, matching camera_scanner / the standalone camera apps; reset
     * to 0 in stop. prev_screen is deleted on success / restored on failure below. */
    lv_obj_t *prev_screen = NULL;
    if (lvgl_port_lock(0)) {
        s_screen = cam_make_black_screen(&prev_screen);
#if !BOARD_CAMERA_PARTITION_MODE
        /* Partition mode drives chrome off LVGL's own refresh timer; the kick is
         * only needed by the image-widget / legacy dummy-draw paths. */
        if (s_screen) {
            board_set_render_interval_ms(10);
        }
#endif
        lvgl_port_unlock();
    }
    if (!s_screen) {
        return "camera screen create failed";
    }

    /* Camera preview pipeline: centered square = the shorter logical dimension
     * (same geometry as camera_scanner). cam_pipeline_create manages its own LVGL
     * access, so it runs OUTSIDE the lock. */
    cam_pipeline_config_t pcfg =
        board_pipeline_default_config(s_screen, board_i2c_get_handle());
    uint32_t square = (BOARD_DISP_H_RES < BOARD_DISP_V_RES)
                          ? BOARD_DISP_H_RES : BOARD_DISP_V_RES;
    pcfg.display_width  = square;
    pcfg.display_height = square;
    /* High-resolution square still (P4). 0 on boards without it → the pipeline
     * allocates no still buffer and the consumer latches the preview square. */
    pcfg.still_width  = BOARD_ENTROPY_STILL_DIM;
    pcfg.still_height = BOARD_ENTROPY_STILL_DIM;

    s_pipeline = cam_pipeline_create(&pcfg);
    if (!s_pipeline) {
        cam_rollback_screen(prev_screen);
        return "pipeline create failed";
    }

    /* Copy any caller seed so the consumer config can point at stable storage. */
    const uint8_t *seed = NULL;
    if (seed_hash && seed_len == 32) {
        memcpy(s_seed, seed_hash, 32);
        seed = s_seed;
    }

    s_frames = 0;
    s_square = square;  /* display/preview square (overlay geometry) */
    /* The latched still is the high-res square when this board grabs one; else it
     * is the preview square (the consumer reports which via get_result's len, but
     * the confirm-image processing below wants the intended dims up front). */
    uint32_t still_dim = BOARD_ENTROPY_STILL_DIM ? BOARD_ENTROPY_STILL_DIM : square;
    s_still_w = still_dim;
    s_still_h = still_dim;
    cam_pipeline_entropy_config_t ecfg = {};
    ecfg.pipeline     = s_pipeline;
    ecfg.frame_width  = square;                    /* preview frames (chained) */
    ecfg.frame_height = square;
    ecfg.still_width  = BOARD_ENTROPY_STILL_DIM;   /* 0 → latch the preview square */
    ecfg.still_height = BOARD_ENTROPY_STILL_DIM;
    ecfg.seed_hash    = seed;
    ecfg.on_frame     = on_entropy_frame;
    ecfg.user_ctx     = NULL;
    s_entropy = cam_pipeline_entropy_create(&ecfg);
    if (!s_entropy) {
        cam_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
        cam_rollback_screen(prev_screen);
        return "entropy consumer create failed";
    }

    /* Overlay over the camera image (created after the pipeline's LVGL image so it draws
     * above it), on our black cam screen. Same centered-square geometry as the camera
     * image / the camera_scanner overlay. Strings are host-provided (empty until the app
     * calls cam_entropy_set_labels); the shutter's camera icon is supplied by the overlay.
     * PREVIEW phase to start. */
    int32_t sq_x = (BOARD_DISP_H_RES - (int32_t)square) / 2;
    int32_t sq_y = (BOARD_DISP_V_RES - (int32_t)square) / 2;
    s_await_confirm = false;
    if (lvgl_port_lock(0)) {
        camera_entropy_overlay_spec_t spec = {};
        spec.square_x        = sq_x;
        spec.square_y        = sq_y;
        spec.square_w        = (int32_t)square;
        spec.square_h        = (int32_t)square;
        spec.capturing_text  = s_capturing_text[0] ? s_capturing_text : NULL;
        spec.accept_label    = s_accept_label[0]   ? s_accept_label   : NULL;
        /* Hardware-mode only; the overlay ignores these under INPUT_MODE_TOUCH. */
        spec.preview_instructions = s_preview_instructions[0] ? s_preview_instructions : NULL;
        spec.confirm_instructions = s_confirm_instructions[0] ? s_confirm_instructions : NULL;
        spec.capture_icon    = NULL;  /* overlay defaults to the camera glyph */
        spec.capture_style   = CAMERA_ENTROPY_CAPTURE_RING;
        spec.phase           = CAMERA_ENTROPY_PHASE_PREVIEW;
        s_overlay = camera_entropy_overlay_create(s_screen, &spec);
        lvgl_port_unlock();
    }
    if (!s_overlay) {
        cam_pipeline_entropy_destroy(s_entropy);
        s_entropy = NULL;
        cam_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
        cam_rollback_screen(prev_screen);
        return "overlay create failed";
    }

    /* Success: the pre-camera screen is no longer needed — the destination View
     * rebuilds it fresh when we return (View-layer contract), and the host's
     * load_screen_and_cleanup_previous only reaps the *active* screen (now ours), so
     * this orphan would otherwise leak. Delete it deliberately here; our black cam
     * screen stays active for the whole session and is itself reaped by the host's
     * next screen load. */
    if (prev_screen && prev_screen != s_screen) {
        if (lvgl_port_lock(0)) {
            lv_obj_delete(prev_screen);
            lvgl_port_unlock();
        }
    }

    s_running = true;
    ESP_LOGI(TAG, "entropy capture started (%ux%u preview square, %ux%u still)",
             (unsigned)square, (unsigned)square, (unsigned)s_still_w, (unsigned)s_still_h);
    return NULL;
}

void cam_entropy_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;

    /* Consumer first (it stops the hashing task + unfreezes the pipeline if a
     * capture left it frozen, and zeroizes its chain/latch). */
    if (s_entropy) {
        cam_pipeline_entropy_destroy(s_entropy);
        s_entropy = NULL;
    }
    /* Overlay handle next — frees ONLY the handle struct; its widgets belong to the
     * screen tree and are stripped below (lv_obj_clean). Then the pipeline (deletes the
     * camera image). */
    if (s_overlay) {
        if (lvgl_port_lock(0)) {
            camera_entropy_overlay_destroy(s_overlay);
            lvgl_port_unlock();
        }
        s_overlay = NULL;
    }
    if (s_pipeline) {
        cam_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
    }
    /* Revert the render interval; and strip any residual overlay widgets so the black cam
     * screen is PURE black through the teardown→next-load gap (Option A). */
    if (lvgl_port_lock(0)) {
        board_set_render_interval_ms(0);
        if (s_screen) {
            lv_obj_clean(s_screen);
        }
        lvgl_port_unlock();
    }
    memset(s_seed, 0, sizeof(s_seed));
    s_frames = 0;
    s_square = 0;
    s_still_w = 0;
    s_still_h = 0;
    s_await_confirm = false;
    /* Option A: leave our black cam screen loaded as the active screen — camera image +
     * overlay widgets gone, it shows black, covering the teardown→next-load gap. We drop
     * our reference WITHOUT deleting the object; the host reaps it via
     * load_screen_and_cleanup_previous when the destination View re-runs. */
    s_screen = NULL;
    ESP_LOGI(TAG, "entropy capture stopped");
}

bool cam_entropy_is_running(void)
{
    return s_running;
}

uint32_t cam_entropy_frames_chained(void)
{
    return s_frames;
}

void cam_entropy_capture(void)
{
    if (s_entropy) {
        cam_pipeline_entropy_capture(s_entropy);
    }
    /* Show the "Capturing…" transient; get_result() flips to CONFIRM once the frozen
     * frame is latched (the overlay's shutter/back hide, so no double-capture). */
    s_await_confirm = true;
    overlay_set_phase(CAMERA_ENTROPY_PHASE_CAPTURING);
}

/* BUILDER-1: on entering CONFIRM, render the latched RAW final still full-display
 * (aspect-FIT — a centered square with pillar bars, box-filter downsampled from the
 * high-res still, plus a luminance contrast stretch, via the screens-repo
 * image_entropy_process) and hand it to the overlay as the review image. The
 * downsample (averaging several sensor pixels per shown pixel) is deliberately what
 * makes the DISPLAYED pixels differ from the HASHED pixels. This is DISPLAY-ONLY:
 * the entropy chain and get_result()'s (chain, frame) stay the RAW latched bytes —
 * processed pixels are never fed back into the entropy hash.
 *
 * Runs on BOTH display architectures. On a partition board (BOARD_CAMERA_PARTITION_MODE,
 * the P4-35) the camera owns the preview square and LVGL's flush is redirected to the
 * gutters, so a full-display LVGL image would be invisible — previously this whole path
 * was compiled out there and that board kept the frozen centre square. It now suspends
 * the fence first: capture() has already frozen the pipeline, so the camera is no longer
 * writing and LVGL can safely own the whole panel for the still review image (and its
 * Accept button). resume() re-fences before unfreezing. board_display_partition_suspend/
 * _resume are no-op stubs on non-partition boards, so the P4-43 path is unchanged. */
/* Deallocator handed to the overlay alongside the confirm-image buffer. The overlay is
 * platform-agnostic (pure LVGL, no ESP-IDF), so it cannot know the frame came from
 * heap_caps_malloc — it just calls this back when the frame is replaced or its widget
 * is deleted. Must match the allocation in push_confirm_image(). */
static void cam_confirm_image_free(void *p)
{
    heap_caps_free(p);
}

static void push_confirm_image(void)
{
    if (!s_entropy || !s_overlay || s_still_w == 0) {
        return;
    }
    const uint8_t *raw = NULL;
    size_t raw_len = 0;
    if (!cam_pipeline_entropy_get_result(s_entropy, NULL, NULL, &raw, &raw_len, NULL) ||
        !raw) {
        return;  /* no latch yet */
    }
    /* The latch is normally the high-res still (s_still_w x s_still_h). If a board
     * without still support — or a still-buffer alloc that fell back — latched the
     * preview square instead, get_result reports the smaller length; process it at
     * the square dims so we never read past the buffer. Anything shorter than even
     * the preview square is unexpected: leave the frozen square. */
    int32_t src_w, src_h;
    if (raw_len >= (size_t)s_still_w * s_still_h * 2) {
        src_w = (int32_t)s_still_w;
        src_h = (int32_t)s_still_h;
    } else if (raw_len >= (size_t)s_square * s_square * 2) {
        src_w = (int32_t)s_square;
        src_h = (int32_t)s_square;
    } else {
        return;
    }
    /* Partition boards: hand the whole panel back to LVGL before drawing anything into
     * the square. Its precondition — a frozen pipeline — is already met, because
     * capture() froze it before the latch we just read. No-op elsewhere. */
    board_display_partition_suspend();
    const int32_t dw = BOARD_DISP_H_RES;
    const int32_t dh = BOARD_DISP_V_RES;
    /* Full-panel RGB565 scratch in PSRAM (same size class as the overlay's own
     * deep copy and the QR A8 mask). Graceful skip on OOM — the overlay keeps the
     * frozen square. */
    uint16_t *disp = (uint16_t *)heap_caps_malloc((size_t)dw * dh * 2, MALLOC_CAP_SPIRAM);
    if (!disp) {
        ESP_LOGW(TAG, "confirm-image alloc failed (%d B SPIRAM)", (int)((size_t)dw * dh * 2));
        return;
    }
    /* Box downscale + a mild unsharp mask (not the plain box the Pi Zero uses) for
     * the P4 confirm image: a cheap edge-acuity "pop" so the frozen review frame
     * reads as crisper than the live preview, without the multi-second cost of a
     * wide bicubic reconstruction. See image_entropy.h. */
    image_entropy_process_filtered(raw, src_w, src_h,
                                   IMAGE_ENTROPY_PIXFMT_RGB565, disp, dw, dh,
                                   IMAGE_ENTROPY_FILTER_BOX_SHARPEN);
    /* HAND THE PSRAM BUFFER OVER — do not copy and do not free it here. The copying
     * setter allocates w*h*2 from the LVGL heap, which is a 128 KB pool
     * (CONFIG_LV_MEM_SIZE_KILOBYTES): a full-display frame is 480*800*2 = 750 KB on the
     * P4-43, so that allocation can never succeed and the setter silently left the
     * frozen square instead of the review image. The _owned form renders straight out
     * of this PSRAM buffer, and the overlay releases it with the deallocator we pass. */
    if (lvgl_port_lock(0)) {
        camera_entropy_overlay_set_confirm_image_owned(s_overlay, disp, dw, dh,
                                                       cam_confirm_image_free);
        lvgl_port_unlock();
        disp = NULL;  /* ownership transferred */
    }
    if (disp) {
        heap_caps_free(disp);  /* never handed over (lock failed) — release it */
    }
}

bool cam_entropy_get_result(const uint8_t **chain, size_t *chain_len,
                            const uint8_t **frame, size_t *frame_len,
                            uint32_t *frames_chained)
{
    if (!s_entropy) {
        return false;
    }
    bool ok = cam_pipeline_entropy_get_result(s_entropy, chain, chain_len,
                                              frame, frame_len, frames_chained);
    /* First successful latch after capture(): advance CAPTURING → CONFIRM (accept /
     * reshoot). One-shot so repeated polls don't re-trigger. Build the full-display
     * review image before flipping the phase so it's ready when CONFIRM reveals it. */
    if (ok && s_await_confirm) {
        s_await_confirm = false;
        push_confirm_image();  /* BUILDER-1 (non-partition boards); no-op otherwise */
        overlay_set_phase(CAMERA_ENTROPY_PHASE_CONFIRM);
    }
    return ok;
}

void cam_entropy_resume(void)
{
    /* Re-fence the camera square BEFORE unfreezing: the camera must not resume writing
     * the square while LVGL still owns the whole panel, or both write the bus at once —
     * the two-writer collision partition mode exists to prevent. No-op on non-partition
     * boards, and a no-op here too if push_confirm_image never suspended. */
    board_display_partition_resume();
    if (s_entropy) {
        cam_pipeline_entropy_resume(s_entropy);
    }
    /* Reshoot: back to the live PREVIEW (shutter + cancel). */
    s_await_confirm = false;
    overlay_set_phase(CAMERA_ENTROPY_PHASE_PREVIEW);
}

#else /* !BOARD_HAS_CAMERA — bindings still link; start() reports the absence. */

const char *cam_entropy_start(const uint8_t *seed_hash, size_t seed_len) { (void)seed_hash; (void)seed_len; return "board has no camera"; }
void cam_entropy_stop(void) {}
void cam_entropy_set_labels(const char *capturing_text, const char *accept_label,
                            const char *preview_instructions, const char *confirm_instructions) {
    (void)capturing_text; (void)accept_label;
    (void)preview_instructions; (void)confirm_instructions;
}
bool cam_entropy_is_running(void) { return false; }
uint32_t cam_entropy_frames_chained(void) { return 0; }
void cam_entropy_capture(void) {}
bool cam_entropy_get_result(const uint8_t **chain, size_t *chain_len,
                            const uint8_t **frame, size_t *frame_len,
                            uint32_t *frames_chained) {
    (void)chain; (void)chain_len; (void)frame; (void)frame_len; (void)frames_chained;
    return false;
}
void cam_entropy_resume(void) {}

#endif /* BOARD_HAS_CAMERA */
