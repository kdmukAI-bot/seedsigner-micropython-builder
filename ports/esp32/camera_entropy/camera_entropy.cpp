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

/* Single live session: one camera, one entropy consumer, tied to start()/stop(). */
static cam_pipeline_handle_t          s_pipeline = NULL;
static cam_pipeline_entropy_handle_t  s_entropy  = NULL;
static lv_obj_t                      *s_screen   = NULL;
static bool                           s_running  = false;
static volatile uint32_t              s_frames   = 0;  /* live progress counter */
static uint8_t                        s_seed[32];      /* copy of optional seed */

/* Fires once per chained preview frame on the consumer task — just record the
 * count for cam_entropy_frames_chained() (must return quickly). */
static void on_entropy_frame(uint32_t frames_chained, void *ctx)
{
    (void)ctx;
    s_frames = frames_chained;
}

const char *cam_entropy_start(const uint8_t *seed_hash, size_t seed_len)
{
    if (s_running) {
        return NULL;  /* idempotent */
    }
    if (seed_hash && seed_len != 0 && seed_len != 32) {
        return "seed_hash must be 32 bytes";
    }

    /* Capture the active screen under the lock (the camera image renders as its
     * child). Short render interval for a responsive real-time preview, matching
     * camera_scanner / the standalone camera apps; reset to 0 in stop. */
    if (lvgl_port_lock(0)) {
        s_screen = lv_screen_active();
        board_set_render_interval_ms(10);
        lvgl_port_unlock();
    }
    if (!s_screen) {
        return "no active screen";
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

    s_pipeline = cam_pipeline_create(&pcfg);
    if (!s_pipeline) {
        return "pipeline create failed";
    }

    /* Copy any caller seed so the consumer config can point at stable storage. */
    const uint8_t *seed = NULL;
    if (seed_hash && seed_len == 32) {
        memcpy(s_seed, seed_hash, 32);
        seed = s_seed;
    }

    s_frames = 0;
    cam_pipeline_entropy_config_t ecfg = {};
    ecfg.pipeline     = s_pipeline;
    ecfg.frame_width  = square;
    ecfg.frame_height = square;
    ecfg.seed_hash    = seed;
    ecfg.on_frame     = on_entropy_frame;
    ecfg.user_ctx     = NULL;
    s_entropy = cam_pipeline_entropy_create(&ecfg);
    if (!s_entropy) {
        cam_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
        return "entropy consumer create failed";
    }

    s_running = true;
    ESP_LOGI(TAG, "entropy capture started (%ux%u square)", (unsigned)square, (unsigned)square);
    return NULL;
}

void cam_entropy_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;

    /* Consumer first (it stops the hashing task + unfreezes the pipeline if a
     * capture left it frozen, and zeroizes its chain/latch), then the pipeline. */
    if (s_entropy) {
        cam_pipeline_entropy_destroy(s_entropy);
        s_entropy = NULL;
    }
    if (s_pipeline) {
        cam_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
    }
    /* Revert the render interval to the idle default (set in cam_entropy_start). */
    if (lvgl_port_lock(0)) {
        board_set_render_interval_ms(0);
        lvgl_port_unlock();
    }
    memset(s_seed, 0, sizeof(s_seed));
    s_frames = 0;
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
}

bool cam_entropy_get_result(const uint8_t **chain, size_t *chain_len,
                            const uint8_t **frame, size_t *frame_len,
                            uint32_t *frames_chained)
{
    if (!s_entropy) {
        return false;
    }
    return cam_pipeline_entropy_get_result(s_entropy, chain, chain_len,
                                           frame, frame_len, frames_chained);
}

void cam_entropy_resume(void)
{
    if (s_entropy) {
        cam_pipeline_entropy_resume(s_entropy);
    }
}

#else /* !BOARD_HAS_CAMERA — bindings still link; start() reports the absence. */

const char *cam_entropy_start(const uint8_t *seed_hash, size_t seed_len) { (void)seed_hash; (void)seed_len; return "board has no camera"; }
void cam_entropy_stop(void) {}
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
