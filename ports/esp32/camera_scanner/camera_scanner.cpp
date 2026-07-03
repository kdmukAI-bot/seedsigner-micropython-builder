/**
 * camera_scanner — builder-side composition of the QR scan pipeline (see
 * camera_scanner.h for the contract). Owns pipeline + overlay + coordinator as a
 * single-session singleton; the MicroPython bindings are a thin skin over the
 * cam_scanner_* C API below.
 */
#include "camera_scanner.h"

#include <string.h>

#include "board.h"
#include "board_config.h"

#include "esp_log.h"

static const char *TAG = "camera_scanner";

#if BOARD_HAS_CAMERA

/* The engine headers lack an extern "C" guard. Force C linkage HERE, before any
 * board header (board_pipeline.h) pulls esp_cam_pipeline.h in under C++ linkage —
 * once that happens its include guard would defeat a later wrap and leave
 * cam_pipeline_create() C++-mangled (link error). Same ordering as qr_overlay_test. */
extern "C" {
#include "esp_cam_pipeline.h"
#include "cam_pipeline_qr.h"
}
#include "scan_coordinator.h"

#include "board_pipeline.h"
#include "board_i2c.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "camera_preview_overlay.h"  /* seedsigner-lvgl-screens overlay (LVGL) */

/* Single live session. The pipeline, overlay, and coordinator have one-of-each
 * lifetimes tied to start()/stop(); there is only ever one camera. */
static cam_pipeline_handle_t      s_pipeline = NULL;
static camera_preview_overlay_t  *s_overlay  = NULL;
static scan_coordinator_t        *s_coord    = NULL;
static lv_obj_t                  *s_screen   = NULL;
static bool                       s_running  = false;

/* ── Injected presenter: maps the neutral scan status to the overlay's visual
 * enum and drives the LVGL widgets. The coordinator already dedups on
 * (status, percent), so this fires only on a real change.
 *
 * THREADING: present() runs on the CONSUMER task (it is called from
 * scan_coordinator_report(), which the MicroPython consumer calls), NOT the decode
 * task. So unlike the old PoC bridge — which ran on the decode task and dropped on
 * lock contention to stay off the flush critical path — here we BLOCK on the lock
 * (timeout 0 = wait): the consumer task is decoupled from the camera/decode rate by
 * the ring, so a brief wait for the LVGL flush is lossless and correct. ── */
static void cam_present(void *ctx, int percent, scan_frame_status_t status)
{
    camera_preview_overlay_t *ov = (camera_preview_overlay_t *)ctx;
    if (!ov) {
        return;
    }

    camera_overlay_frame_status_t dot;
    switch (status) {
    case SCAN_FRAME_NEW:    dot = CAMERA_OVERLAY_FRAME_ADDED;    break;  /* green  */
    case SCAN_FRAME_REPEAT: dot = CAMERA_OVERLAY_FRAME_REPEATED; break;  /* gray   */
    case SCAN_FRAME_MISS:   /* hidden for now — sustained-MISS warning is Python's (§6) */
    case SCAN_FRAME_NONE:
    default:                dot = CAMERA_OVERLAY_FRAME_NONE;     break;  /* hidden */
    }

    if (lvgl_port_lock(0)) {
        camera_preview_overlay_set_progress(ov, percent, dot);
        lvgl_port_unlock();
    }
}

/* ── Completion (terminal, once). The coordinator emits the final NEW@100% present
 * just before this, so the bar is already full + green; this is just the terminal
 * marker. The Python consumer drives its own post-complete handoff (it called
 * report_complete), so nothing more is needed here than a log. ── */
static void cam_complete(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "scan complete");
}

const char *cam_scanner_start(void)
{
    if (s_running) {
        return NULL;  /* idempotent */
    }

    /* Capture the active screen under the lock (the camera image + overlay become
     * its children, so the overlay draws above the live preview). Also set a short
     * LVGL render interval for a responsive real-time preview, matching the
     * standalone camera apps (qr_overlay_test, scan_coord_test); reset to 0 in
     * cam_scanner_stop. (The overlay-lock starvation that used to hang start() is
     * fixed by flattening the lvgl/CSI task priorities to 1 — see board_common.) */
    if (lvgl_port_lock(0)) {
        s_screen = lv_screen_active();
        board_set_render_interval_ms(10);
        lvgl_port_unlock();
    }
    if (!s_screen) {
        return "no active screen";
    }

    /* Camera preview pipeline: centered square = the shorter logical dimension
     * (the display sink centers it on the panel; landscape gutters stay static).
     * cam_pipeline_create manages its own LVGL access, so it runs OUTSIDE the lock
     * (same as qr_overlay_test). */
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

    /* Overlay over the camera image: created after the pipeline's LVGL image so it
     * draws above it. Geometry matches the centered square so the in-square status
     * bar sits over the camera and the gutter back button lands in the static
     * margin (never over live pixels). */
    int32_t sq_x = (BOARD_DISP_H_RES - (int32_t)square) / 2;
    int32_t sq_y = (BOARD_DISP_V_RES - (int32_t)square) / 2;
    if (lvgl_port_lock(0)) {
        camera_preview_overlay_spec_t spec = {};
        spec.instructions_text = "< back  |  Scan a QR code";  /* ignored in touch mode */
        spec.square_x = sq_x;
        spec.square_y = sq_y;
        spec.square_w = (int32_t)square;
        spec.square_h = (int32_t)square;
        spec.scanning_active = true;  /* show the status bar from the start */
        spec.progress_percent = 0;
        spec.frame_status = CAMERA_OVERLAY_FRAME_NONE;
        s_overlay = camera_preview_overlay_create(s_screen, &spec);
        lvgl_port_unlock();
    }
    if (!s_overlay) {
        cam_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
        return "overlay create failed";
    }

    /* Coordinator: engine per-frame outcome -> transport-dedup -> NEW ring +
     * status cell; report() -> cam_present -> overlay. */
    scan_coordinator_config_t ccfg = {};
    ccfg.pipeline       = s_pipeline;
    ccfg.frame_width    = square;
    ccfg.frame_height   = square;
    ccfg.present        = cam_present;
    ccfg.present_ctx    = s_overlay;
    ccfg.on_complete    = cam_complete;
    ccfg.complete_ctx   = s_overlay;
    ccfg.new_ring_depth = 0;  /* default */
    s_coord = scan_coordinator_create(&ccfg);
    if (!s_coord) {
        if (lvgl_port_lock(0)) {
            camera_preview_overlay_destroy(s_overlay);
            lvgl_port_unlock();
        }
        s_overlay = NULL;
        cam_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
        return "scan_coordinator create failed";
    }

    s_running = true;
    ESP_LOGI(TAG, "scanner started (%ux%u square)", (unsigned)square, (unsigned)square);
    return NULL;
}

void cam_scanner_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;

    /* Coordinator first: it stops the QR consumer (no more present() calls) before
     * we free the overlay it points at. It does NOT touch the pipeline. */
    if (s_coord) {
        scan_coordinator_destroy(s_coord);
        s_coord = NULL;
    }
    if (s_overlay) {
        if (lvgl_port_lock(0)) {
            camera_preview_overlay_destroy(s_overlay);
            lvgl_port_unlock();
        }
        s_overlay = NULL;
    }
    if (s_pipeline) {
        cam_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
    }
    /* Revert the render interval to the idle default (set in cam_scanner_start). */
    if (lvgl_port_lock(0)) {
        board_set_render_interval_ms(0);
        lvgl_port_unlock();
    }
    s_screen = NULL;
    ESP_LOGI(TAG, "scanner stopped");
}

bool cam_scanner_is_running(void)
{
    return s_running;
}

bool cam_scanner_poll_new(const uint8_t **payload, size_t *len)
{
    if (!s_coord || !payload || !len) {
        return false;
    }
    scan_new_event_t ev;
    if (!scan_coordinator_poll_new(s_coord, &ev)) {
        return false;
    }
    *payload = ev.payload;
    *len = ev.len;
    return true;
}

void cam_scanner_read_status(cam_scanner_status_t *out)
{
    if (!out) {
        return;
    }
    *out = cam_scanner_status_t{};
    if (!s_coord) {
        return;
    }
    scan_status_t st;
    scan_coordinator_read_status(s_coord, &st);
    out->latest             = (int)st.latest;
    out->consecutive_misses = st.consecutive_misses;
    out->dropped_new        = st.dropped_new;
    out->has_corners        = st.has_corners;  /* false until engine plumbs corners */
}

void cam_scanner_report(int status, int percent)
{
    if (!s_coord) {
        return;
    }
    scan_coordinator_report(s_coord, (scan_frame_status_t)status, percent);
}

void cam_scanner_report_complete(void)
{
    if (!s_coord) {
        return;
    }
    scan_coordinator_report_complete(s_coord);
}

#else /* !BOARD_HAS_CAMERA — bindings still link; start() reports the absence. */

const char *cam_scanner_start(void) { return "board has no camera"; }
void cam_scanner_stop(void) {}
bool cam_scanner_is_running(void) { return false; }
bool cam_scanner_poll_new(const uint8_t **payload, size_t *len) { (void)payload; (void)len; return false; }
void cam_scanner_read_status(cam_scanner_status_t *out) { if (out) { memset(out, 0, sizeof(*out)); } }
void cam_scanner_report(int status, int percent) { (void)status; (void)percent; }
void cam_scanner_report_complete(void) {}

#endif /* BOARD_HAS_CAMERA */
