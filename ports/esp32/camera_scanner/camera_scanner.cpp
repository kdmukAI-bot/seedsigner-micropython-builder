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

/* Partition mode (P4-35 ST7796): LVGL keeps running during the preview so the
 * side gutters render live chrome with working touch. Default off for boards
 * that use the LVGL-stopped dummy-draw path. */
#ifndef BOARD_CAMERA_PARTITION_MODE
#define BOARD_CAMERA_PARTITION_MODE 0
#endif

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
#include "board_pipeline_display_lvgl.h"  /* portrait_direct config fields */
#include "board_i2c.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "camera_preview_overlay.h"     /* seedsigner-lvgl-screens landscape overlay (LVGL) */
#include "camera_preview_pillarboxed.h" /* seedsigner-lvgl-screens portrait-mount pillarboxed chrome */
#include "components.h"                 /* seedsigner-lvgl-screens back_button() */

/* Portrait scan display (Phase 1): on the ST7701/DSI 4.3, a QR-scan session
 * renders the SCAN SCREEN in native portrait (no per-frame 90° rotate) so the
 * camera direct-blits the centered square and core 0 frees for a 2nd decoder.
 * Focus-assist and entropy stay landscape image-widget. 4.3-only. */
#if BOARD_DISPLAY_DRIVER == DISPLAY_ST7701
#define SCAN_USES_PORTRAIT 1
#else
#define SCAN_USES_PORTRAIT 0
#endif

/* Single live session. The pipeline, overlay, and coordinator have one-of-each
 * lifetimes tied to start()/stop(); there is only ever one camera. */
static cam_pipeline_handle_t      s_pipeline = NULL;
static camera_preview_overlay_t  *s_overlay  = NULL;
static scan_coordinator_t        *s_coord    = NULL;
static lv_obj_t                  *s_screen   = NULL;
static bool                       s_running  = false;

#if SCAN_USES_PORTRAIT
/* Portrait scan session state + the pillarboxed camera-preview chrome (progress bar,
 * status dot, percent, back button) drawn in the letterbox strips beside the camera
 * square. The chrome is the portable seedsigner-lvgl-screens camera_preview_pillarboxed
 * module: authored in native portrait so the physical mount presents it as landscape
 * (pre-rotated percent; a CHEVRON_DOWN back button that reads "left"). The back button
 * reuses the screens factory, so a tap posts the same SEEDSIGNER_RET_BACK_BUTTON the
 * app's scan-cancel path drains. */
#define PV_SQ       (BOARD_LCD_H_RES)                 /* 480 square side       */
#define PV_SY       ((BOARD_LCD_V_RES - PV_SQ) / 2)   /* 160 square top row    */
static bool                          s_scan_portrait = false;
static camera_preview_pillarboxed_t *s_pv_chrome     = NULL;

/* One-time black-square clear: the FB holds the stale rotated landscape frame
 * until the first camera frame (~500 ms warmup); the reserved-rect keeps LVGL
 * from painting the square, so clear it directly. Caller need not hold the lock. */
static void cam_portrait_black_clear(void)
{
    size_t n = (size_t)PV_SQ * PV_SQ * 2;
    uint16_t *black = (uint16_t *)heap_caps_aligned_alloc(
        128, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (black) {
        memset(black, 0, n);
        board_display_portrait_scan_blit(0, PV_SY, PV_SQ, PV_SY + PV_SQ, black);
        heap_caps_free(black);
    }
}

/* Build the pillarboxed chrome on s_screen (already resized to the portrait panel). The
 * module paints the letterbox strips, the vertical progress bar, the status dot, the
 * pre-rotated percent, and the CHEVRON_DOWN back button — all authored in portrait, sized
 * from the camera square + the live display resolution. Caller holds the LVGL port lock. */
static void cam_portrait_chrome_create(lv_obj_t *scr)
{
    camera_preview_pillarboxed_spec_t spec = {};
    spec.square_x = 0;
    spec.square_y = PV_SY;
    spec.square_w = PV_SQ;
    spec.square_h = PV_SQ;
    spec.scanning_active  = true;
    spec.progress_percent = 0;
    spec.frame_status     = CAMERA_OVERLAY_FRAME_NONE;
    s_pv_chrome = camera_preview_pillarboxed_create(scr, &spec);
}

/* Free the chrome handle. The widgets are children of s_screen and reaped when the host
 * reaps the screen (the rot_text buffers free themselves via the image delete cb); this
 * frees the handle struct and drops the pointer so a late present() can't touch it. */
static void cam_portrait_chrome_forget(void)
{
    camera_preview_pillarboxed_destroy(s_pv_chrome);
    s_pv_chrome = NULL;
}

/* Teardown before board_display_exit_portrait_scan(): forget the handle AND strip the
 * portrait-authored chrome off s_screen. exit_portrait_scan() restores landscape mode and
 * invalidates the active screen, re-rendering it through the landscape 90° rotate path — the
 * chrome would otherwise flash rotated 90° CCW for a frame (brief on animated QRs; lingers on
 * static ones, since nothing navigates away as quickly) until the app loads the next screen.
 * Cleaning s_screen first leaves a plain black transient. Caller must NOT hold the LVGL lock. */
static void cam_portrait_chrome_teardown(void)
{
    cam_portrait_chrome_forget();
    if (s_screen && lvgl_port_lock(0)) {
        lv_obj_clean(s_screen);
        lvgl_port_unlock();
    }
}
#endif /* SCAN_USES_PORTRAIT */

/* Focus-assist session (see cam_scanner_start(focus_assist=true)). Mutually
 * exclusive with the scan overlay/coordinator above: this path skips both and
 * runs the camera preview with an on-screen software focus meter driven off a
 * focus-only QR consumer (quirc skipped; Laplacian sharpness instead). */
static bool                       s_focus_assist = false;
static cam_pipeline_qr_handle_t   s_focus_qr     = NULL;
static lv_timer_t                *s_focus_timer  = NULL;
static lv_obj_t                  *s_focus_bar    = NULL;  /* live sharpness bar    */
static lv_obj_t                  *s_focus_peak   = NULL;  /* peak-hold marker      */
static lv_obj_t                  *s_focus_label  = NULL;  /* status text           */
static float                      s_focus_scale  = 1.0f;  /* auto-range reference  */
static int32_t                    s_focus_bar_x  = 0;     /* bar geometry (marker) */
static int32_t                    s_focus_bar_y  = 0;
static int32_t                    s_focus_bar_w  = 0;
static int32_t                    s_focus_bar_h  = 0;

#if BOARD_CAMERA_PARTITION_MODE
/* M1 partition-mode proof: a live progress widget in the right gutter (updated
 * every present()) demonstrating non-camera LVGL chrome rendering beside the
 * streaming square with LVGL running + touch alive. Throwaway scaffolding — M3
 * replaces it with the real gutter layout in camera_preview_overlay (screens). */
static lv_obj_t                  *s_gutter_bar = NULL;
static lv_obj_t                  *s_gutter_pct = NULL;
#endif

/* ── Option A dedicated camera screen ───────────────────────────────────────────
 * The camera image + overlay render onto their OWN opaque-black screen rather than
 * over the previously-active one. Loading it immediately means the camera warm-up
 * window (and, on stop, the teardown→next-load gap) shows black instead of the
 * screen the user came from — see docs/camera-pipeline-teardown-screen-reveal.md. ── */

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
#if SCAN_USES_PORTRAIT
    /* Restore landscape before loading the pre-camera (landscape) screen. */
    if (s_scan_portrait) {
        cam_portrait_chrome_teardown();
        board_display_exit_portrait_scan();
        s_scan_portrait = false;
    }
#endif
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
    /* Map the neutral scan status to the overlay dot enum (shared by both presenters).
     * NONE/MISS: the pillarboxed chrome shows an EMPTY ring; the landscape overlay hides
     * the dot. Sustained-MISS warning is Python's concern (§6). */
    camera_overlay_frame_status_t dot;
    switch (status) {
    case SCAN_FRAME_NEW:    dot = CAMERA_OVERLAY_FRAME_ADDED;    break;  /* green */
    case SCAN_FRAME_REPEAT: dot = CAMERA_OVERLAY_FRAME_REPEATED; break;  /* gray  */
    case SCAN_FRAME_MISS:
    case SCAN_FRAME_NONE:
    default:                dot = CAMERA_OVERLAY_FRAME_NONE;     break;  /* empty / hidden */
    }

#if SCAN_USES_PORTRAIT
    if (s_scan_portrait) {
        if (lvgl_port_lock(0)) {
            camera_preview_pillarboxed_set_progress(s_pv_chrome, percent, dot);
            lvgl_port_unlock();
        }
        return;
    }
#endif
    camera_preview_overlay_t *ov = (camera_preview_overlay_t *)ctx;
    if (!ov) {
        return;
    }

    if (lvgl_port_lock(0)) {
        camera_preview_overlay_set_progress(ov, percent, dot);
#if BOARD_CAMERA_PARTITION_MODE
        if (s_gutter_bar) lv_bar_set_value(s_gutter_bar, percent, LV_ANIM_OFF);
        if (s_gutter_pct) lv_label_set_text_fmt(s_gutter_pct, "%d%%", percent);
#endif
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

#if BOARD_CAMERA_PARTITION_MODE
/* ── M1 gutter chrome (placeholder) ──────────────────────────────────────────
 * On-device proof of the partition mechanism: live, non-camera LVGL widgets in
 * the right gutter that keep updating (with the left-gutter back button
 * touchable) while the camera direct-blits the square. Throwaway scaffolding —
 * M3 replaces it with the real cancel/percent/vertical-bar gutter layout in the
 * camera_preview_overlay screen. Caller holds the LVGL port lock. */
static void cam_gutter_placeholder_create(lv_obj_t *parent, int32_t sq_x, int32_t square)
{
    int32_t rg_x = sq_x + square;                       /* right gutter left edge  */
    int32_t rg_w = (int32_t)BOARD_DISP_H_RES - rg_x;    /* right gutter width (~80) */
    if (rg_w <= 0) return;                              /* square fills the panel   */
    int32_t cx = rg_x + rg_w / 2;

    s_gutter_pct = lv_label_create(parent);
    lv_label_set_text(s_gutter_pct, "0%");
    lv_obj_set_style_text_color(s_gutter_pct, lv_color_white(), 0);
    lv_obj_align(s_gutter_pct, LV_ALIGN_TOP_LEFT, cx - 16, 12);

    int32_t bar_w = 20;
    int32_t bar_h = (int32_t)BOARD_DISP_V_RES - 60;     /* taller-than-wide => vertical */
    s_gutter_bar = lv_bar_create(parent);
    lv_obj_set_size(s_gutter_bar, bar_w, bar_h);
    lv_obj_set_pos(s_gutter_bar, cx - bar_w / 2, 44);
    lv_bar_set_range(s_gutter_bar, 0, 100);
    lv_bar_set_value(s_gutter_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_gutter_bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_gutter_bar, lv_color_hex(0xF08C00), LV_PART_INDICATOR);
}

static void cam_gutter_placeholder_destroy(void)
{
    if (s_gutter_bar) { lv_obj_delete(s_gutter_bar); s_gutter_bar = NULL; }
    if (s_gutter_pct) { lv_obj_delete(s_gutter_pct); s_gutter_pct = NULL; }
}
#endif /* BOARD_CAMERA_PARTITION_MODE */

/* ── Focus-assist on-screen meter ────────────────────────────────────────────
 * A horizontal sharpness bar overlaid near the bottom of the camera square,
 * plus a peak-hold marker and a short status label. Updated ~5 Hz from an
 * lv_timer that polls cam_pipeline_qr_get_focus_metric() (the focus consumer
 * writes the metric on the decode task). The metric's absolute scale is
 * scene-dependent, so the bar auto-ranges against the running peak: the marker
 * shows the sharpest point recently passed, the live bar rises to meet it at
 * best focus. v1 targets the DSI/LVGL path (P4-43), where LVGL composites these
 * widgets over the camera image widget. On partition-mode SPI boards the camera
 * direct-blits the square and would paint over these widgets — a gutter-placed
 * variant is a follow-up (see docs/camera-pipeline-debug-hud-brief.md).
 *
 * All three functions require the caller to hold the esp_lvgl_port lock, EXCEPT
 * the lv_timer callback, which the port already runs under the lock. ── */
static void cam_focus_hud_create(lv_obj_t *parent, int32_t sq_x, int32_t sq_y,
                                 int32_t square)
{
    int32_t margin = square / 10;
    s_focus_bar_w = square - 2 * margin;
    s_focus_bar_h = 26;
    s_focus_bar_x = sq_x + margin;
    s_focus_bar_y = sq_y + square - margin - s_focus_bar_h;

    s_focus_label = lv_label_create(parent);
    lv_label_set_text(s_focus_label, "FOCUS");
    lv_obj_set_style_text_color(s_focus_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_focus_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_focus_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(s_focus_label, 4, 0);
    lv_obj_set_pos(s_focus_label, s_focus_bar_x, s_focus_bar_y - 36);

    s_focus_bar = lv_bar_create(parent);
    lv_obj_set_pos(s_focus_bar, s_focus_bar_x, s_focus_bar_y);
    lv_obj_set_size(s_focus_bar, s_focus_bar_w, s_focus_bar_h);
    lv_bar_set_range(s_focus_bar, 0, 1000);
    lv_bar_set_value(s_focus_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_focus_bar, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_focus_bar, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_focus_bar, lv_color_hex(0xF08C00), LV_PART_INDICATOR);

    /* Peak-hold marker: a thin bright vertical bar we slide along the track. */
    s_focus_peak = lv_obj_create(parent);
    lv_obj_remove_style_all(s_focus_peak);
    lv_obj_set_size(s_focus_peak, 3, s_focus_bar_h);
    lv_obj_set_style_bg_color(s_focus_peak, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_focus_peak, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_focus_peak, s_focus_bar_x, s_focus_bar_y);
}

static void cam_focus_hud_update(lv_timer_t *timer)
{
    (void)timer;
    if (!s_focus_qr || !s_focus_bar) {
        return;
    }
    cam_pipeline_qr_focus_t f;
    if (!cam_pipeline_qr_get_focus_metric(s_focus_qr, &f)) {
        return;  /* no frame processed yet */
    }

    /* Auto-range against the running peak (slow decay keeps the bar using the
     * full height as the scene's achievable max drifts). */
    if (f.peak > s_focus_scale) {
        s_focus_scale = f.peak;
    } else {
        s_focus_scale += (f.peak - s_focus_scale) * 0.02f;
    }
    if (s_focus_scale < 1.0f) {
        s_focus_scale = 1.0f;
    }

    int32_t live = (int32_t)(1000.0f * f.sharpness / s_focus_scale);
    int32_t peak = (int32_t)(1000.0f * f.peak / s_focus_scale);
    if (live < 0) live = 0; else if (live > 1000) live = 1000;
    if (peak < 0) peak = 0; else if (peak > 1000) peak = 1000;

    lv_bar_set_value(s_focus_bar, live, LV_ANIM_OFF);
    int32_t mx = s_focus_bar_x + (int32_t)((int64_t)peak * (s_focus_bar_w - 3) / 1000);
    lv_obj_set_x(s_focus_peak, mx);

    /* Green when the live bar is within ~4% of the recent peak (at the sweet
     * spot); amber while hunting; and a light warning when the exposure is off. */
    bool at_peak = f.usable && (peak - live) < 40;
    lv_obj_set_style_bg_color(s_focus_bar,
        at_peak ? lv_color_hex(0x2FB344) : lv_color_hex(0xF08C00),
        LV_PART_INDICATOR);

    /* Plain ASCII: the montserrat font subset lacks em-dash / arrow glyphs. */
    if (!f.usable) {
        lv_label_set_text(s_focus_label,
                          f.luma_mean < 18 ? "FOCUS - too dark"
                                           : "FOCUS - too bright");
    } else if (at_peak) {
        lv_label_set_text(s_focus_label, "FOCUS - SHARP");
    } else {
        lv_label_set_text_fmt(s_focus_label, "FOCUS  %d%%", (int)(live / 10));
    }
}

static void cam_focus_hud_destroy_widgets(void)
{
    if (s_focus_peak)  { lv_obj_delete(s_focus_peak);  s_focus_peak  = NULL; }
    if (s_focus_bar)   { lv_obj_delete(s_focus_bar);   s_focus_bar   = NULL; }
    if (s_focus_label) { lv_obj_delete(s_focus_label); s_focus_label = NULL; }
}

const char *cam_scanner_start(bool focus_assist)
{
    if (s_running) {
        return NULL;  /* idempotent */
    }

    /* Build on our OWN opaque-black screen (Option A) rather than the active one, so
     * the camera warm-up window shows black instead of the screen the user came from
     * — the camera image + overlay become its children, so the overlay draws above
     * the live preview. Also set a short LVGL render interval for a responsive
     * real-time preview, matching the standalone camera apps (qr_overlay_test,
     * scan_coord_test); reset to 0 in cam_scanner_stop. (The overlay-lock starvation
     * that used to hang start() is fixed by flattening the lvgl/CSI task priorities
     * to 1 — see board_common.) prev_screen is deleted on success / restored on
     * failure below. */
    lv_obj_t *prev_screen = NULL;
    if (lvgl_port_lock(0)) {
        s_screen = cam_make_black_screen(&prev_screen);
#if !BOARD_CAMERA_PARTITION_MODE
        /* Image-widget / legacy dummy-draw paths want a responsive render kick.
         * Partition mode drives its gutter chrome off LVGL's own ~33ms refresh
         * timer instead, so the kick is dropped to keep the camera's blit lock
         * uncontended (the preview is direct-blit and never LVGL-driven). */
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
     * (the display sink centers it on the panel; landscape gutters stay static).
     * cam_pipeline_create manages its own LVGL access, so it runs OUTSIDE the lock
     * (same as qr_overlay_test). */
    cam_pipeline_config_t pcfg =
        board_pipeline_default_config(s_screen, board_i2c_get_handle());
    uint32_t square = (BOARD_DISP_H_RES < BOARD_DISP_V_RES)
                          ? BOARD_DISP_H_RES : BOARD_DISP_V_RES;
    pcfg.display_width  = square;
    pcfg.display_height = square;

    /* Per-session mode: the QR scan renders in native portrait (direct-blit,
     * no rotate) to free core 0; focus-assist stays landscape image-widget. */
#if SCAN_USES_PORTRAIT
    bool use_portrait = false;
    if (!focus_assist) {
        use_portrait = true;
        board_display_enter_portrait_scan();
        s_scan_portrait = true;
        if (lvgl_port_lock(0)) {
            /* s_screen was created landscape-sized; match the portrait panel. */
            lv_obj_set_size(s_screen, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
            /* Skip the render-kick: the camera direct-blits the square and the
             * letterbox updates via LVGL's own timer, so keep core 0 idle. */
            board_set_render_interval_ms(0);
            lvgl_port_unlock();
        }
        pcfg.rotation = 0;  /* native portrait ⇒ no PPA rotation */
        board_pipeline_lvgl_display_config_t *dc =
            (board_pipeline_lvgl_display_config_t *)pcfg.display_config;
        dc->portrait_direct = true;
        dc->portrait_x = 0;
        dc->portrait_y = (int32_t)(BOARD_LCD_V_RES - (int32_t)square) / 2;
    }
#endif

    s_pipeline = cam_pipeline_create(&pcfg);
    if (!s_pipeline) {
        cam_rollback_screen(prev_screen);
        return "pipeline create failed";
    }

    /* Overlay over the camera image: created after the pipeline's LVGL image so it
     * draws above it. Geometry matches the centered square so the in-square status
     * bar sits over the camera and the gutter back button lands in the static
     * margin (never over live pixels). */
    int32_t sq_x = (BOARD_DISP_H_RES - (int32_t)square) / 2;
    int32_t sq_y = (BOARD_DISP_V_RES - (int32_t)square) / 2;

    /* ── Focus-assist session: preview + software focus meter, no scan overlay
     * or coordinator. A focus-only QR consumer (quirc skipped) writes the
     * sharpness metric; the HUD timer renders it. ── */
    if (focus_assist) {
        cam_pipeline_qr_config_t fcfg = {};
        fcfg.pipeline     = s_pipeline;
        fcfg.frame_width  = square;
        fcfg.frame_height = square;
        fcfg.on_frame     = NULL;   /* optional in focus mode */
        fcfg.user_ctx     = NULL;
        fcfg.focus_assist = true;
        s_focus_qr = cam_pipeline_qr_create(&fcfg);
        if (!s_focus_qr) {
            cam_pipeline_destroy(s_pipeline);
            s_pipeline = NULL;
            cam_rollback_screen(prev_screen);
            return "focus consumer create failed";
        }

        /* Reuse the scan overlay purely for its persistent touch back button (it
         * lives in the top-left gutter and posts to the same UI event queue the
         * app's scan-cancel path drains, so exit works identically). scanning_active
         * = false hides the scan status bar + dot, so the focus meter is the only
         * in-square chrome; the gutter back button doesn't overlap it. */
        if (lvgl_port_lock(0)) {
            camera_preview_overlay_spec_t spec = {};
            spec.instructions_text = "< back  |  Focus the camera";  /* ignored in touch mode */
            spec.square_x = sq_x;
            spec.square_y = sq_y;
            spec.square_w = (int32_t)square;
            spec.square_h = (int32_t)square;
            spec.scanning_active = false;  /* back affordance only, no scan status */
            spec.progress_percent = 0;
            spec.frame_status = CAMERA_OVERLAY_FRAME_NONE;
            s_overlay = camera_preview_overlay_create(s_screen, &spec);
            lvgl_port_unlock();
        }
        if (!s_overlay) {
            cam_pipeline_qr_destroy(s_focus_qr);
            s_focus_qr = NULL;
            cam_pipeline_destroy(s_pipeline);
            s_pipeline = NULL;
            cam_rollback_screen(prev_screen);
            return "focus overlay create failed";
        }

        /* Focus meter on top of the overlay (created after it → higher z-order). */
        if (lvgl_port_lock(0)) {
            cam_focus_hud_create(s_screen, sq_x, sq_y, (int32_t)square);
            s_focus_scale = 1.0f;
            s_focus_timer = lv_timer_create(cam_focus_hud_update, 200, NULL);
            lvgl_port_unlock();
        }

        /* Success: drop the pre-camera screen (see the scan path's rationale). */
        if (prev_screen && prev_screen != s_screen) {
            if (lvgl_port_lock(0)) {
                lv_obj_delete(prev_screen);
                lvgl_port_unlock();
            }
        }

        s_focus_assist = true;
        s_running = true;
        ESP_LOGI(TAG, "focus-assist started (%ux%u square)",
                 (unsigned)square, (unsigned)square);
        return NULL;
    }

#if SCAN_USES_PORTRAIT
    if (use_portrait) {
        /* Portrait scan: clear the stale square, then a minimal C-side letterbox
         * overlay (no screens-repo overlay — that's landscape image-widget). */
        cam_portrait_black_clear();
        if (lvgl_port_lock(0)) {
            cam_portrait_chrome_create(s_screen);
            lvgl_port_unlock();
        }
    } else
#endif
    {
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
            cam_rollback_screen(prev_screen);
            return "overlay create failed";
        }
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
    /* A 2nd decoder pays off only where its core is otherwise free: the portrait
     * DSI scan drops the per-frame 90° rotate off core 0. Landscape image-widget
     * sessions and SPI-partition boards keep core 0 busy, so they stay single. */
    ccfg.num_decoders   = 1;
#if SCAN_USES_PORTRAIT
    if (use_portrait) {
        ccfg.num_decoders = 2;
    }
#endif
    s_coord = scan_coordinator_create(&ccfg);
    if (!s_coord) {
        if (s_overlay && lvgl_port_lock(0)) {
            camera_preview_overlay_destroy(s_overlay);
            lvgl_port_unlock();
        }
        s_overlay = NULL;
#if SCAN_USES_PORTRAIT
        cam_portrait_chrome_forget();  /* widgets reaped with s_screen in rollback */
#endif
        cam_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
        cam_rollback_screen(prev_screen);
        return "scan_coordinator create failed";
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

#if BOARD_CAMERA_PARTITION_MODE
    /* Add the live gutter placeholder now that the session is fully built. */
    if (lvgl_port_lock(0)) {
        cam_gutter_placeholder_create(s_screen, sq_x, (int32_t)square);
        lvgl_port_unlock();
    }
#endif

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

    /* ── Focus-assist teardown ──────────────────────────────────────────────
     * Delete the HUD timer FIRST (under the lock, so no further callback reads
     * s_focus_qr / the widgets), then stop the focus consumer (off the lock —
     * its destroy does a task handshake and must not hold the LVGL lock), then
     * drop the widgets, then the shared pipeline/render/screen tail. ── */
    if (s_focus_assist) {
        if (lvgl_port_lock(0)) {
            if (s_focus_timer) {
                lv_timer_delete(s_focus_timer);
                s_focus_timer = NULL;
            }
            lvgl_port_unlock();
        }
        if (s_focus_qr) {
            cam_pipeline_qr_destroy(s_focus_qr);
            s_focus_qr = NULL;
        }
        if (lvgl_port_lock(0)) {
            cam_focus_hud_destroy_widgets();
            if (s_overlay) {
                camera_preview_overlay_destroy(s_overlay);
                s_overlay = NULL;
            }
            lvgl_port_unlock();
        }
        if (s_pipeline) {
            cam_pipeline_destroy(s_pipeline);
            s_pipeline = NULL;
        }
        if (lvgl_port_lock(0)) {
            board_set_render_interval_ms(0);
            lvgl_port_unlock();
        }
        s_screen = NULL;
        s_focus_assist = false;
        ESP_LOGI(TAG, "focus-assist stopped");
        return;
    }

    /* Coordinator first: it stops the QR consumer (no more present() calls) before
     * we free the overlay it points at. It does NOT touch the pipeline. */
    if (s_coord) {
        scan_coordinator_destroy(s_coord);
        s_coord = NULL;
    }
    if (s_overlay) {
        if (lvgl_port_lock(0)) {
            camera_preview_overlay_destroy(s_overlay);
#if BOARD_CAMERA_PARTITION_MODE
            cam_gutter_placeholder_destroy();
#endif
            lvgl_port_unlock();
        }
        s_overlay = NULL;
    }
    if (s_pipeline) {
        cam_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
    }
#if SCAN_USES_PORTRAIT
    /* Restore landscape before the next screen render (also covers the Python-
     * exception teardown path, which routes through cam_scanner_stop). */
    if (s_scan_portrait) {
        cam_portrait_chrome_teardown();
        board_display_exit_portrait_scan();
        s_scan_portrait = false;
    }
#endif
    /* Revert the render interval to the idle default (set in cam_scanner_start). */
    if (lvgl_port_lock(0)) {
        board_set_render_interval_ms(0);
        lvgl_port_unlock();
    }
    /* Option A: leave our black cam screen loaded as the active screen — with the
     * camera image gone it shows black, covering the teardown→next-load gap so the
     * pre-camera screen is never revealed. We drop our reference WITHOUT deleting the
     * object; the host reaps it via load_screen_and_cleanup_previous when the
     * destination View re-runs and loads its own screen. */
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

bool cam_scanner_poll_miss_frame(const uint8_t **payload, size_t *len,
                                 cam_scanner_miss_meta_t *out)
{
    if (!s_coord || !payload || !len || !out) {
        return false;
    }
    cam_pipeline_qr_handle_t qr = scan_coordinator_qr_handle(s_coord);
    if (!qr) {
        return false;
    }
    cam_pipeline_qr_miss_meta_t m;
    const uint8_t *buf = NULL;
    size_t n = 0;
    if (!cam_pipeline_qr_poll_miss_frame(qr, &buf, &n, &m)) {
        return false;
    }
    *payload = buf;
    *len = n;
    out->seq          = m.seq;
    out->timestamp_us = m.timestamp_us;
    out->quirc_err    = m.quirc_err;
    out->side_px      = m.side_px;
    out->sharpness    = m.sharpness;
    out->luma_mean    = m.luma_mean;
    out->width        = m.width;
    out->height       = m.height;
    return true;
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

const char *cam_scanner_start(bool focus_assist) { (void)focus_assist; return "board has no camera"; }
void cam_scanner_stop(void) {}
bool cam_scanner_is_running(void) { return false; }
bool cam_scanner_poll_new(const uint8_t **payload, size_t *len) { (void)payload; (void)len; return false; }
void cam_scanner_read_status(cam_scanner_status_t *out) { if (out) { memset(out, 0, sizeof(*out)); } }
bool cam_scanner_poll_miss_frame(const uint8_t **payload, size_t *len, cam_scanner_miss_meta_t *meta) { (void)payload; (void)len; (void)meta; return false; }
void cam_scanner_report(int status, int percent) { (void)status; (void)percent; }
void cam_scanner_report_complete(void) {}

#endif /* BOARD_HAS_CAMERA */
