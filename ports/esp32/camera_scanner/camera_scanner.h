/*
 * camera_scanner — builder-owned MicroPython-facing glue for the QR scan pipeline.
 *
 * The thinnest composition that can ONLY live in the builder, where all three deps
 * already sit:
 *
 *   esp-camera-pipeline (engine)  --per-frame outcome-->  scan_coordinator
 *     (esp-board-common: transport-dedup + NEW ring + status cell + consecutive_misses)
 *       --neutral (percent, status)-->  THIS presenter  -->  camera_preview_overlay
 *         (seedsigner-lvgl-screens LVGL widgets, under the esp_lvgl_port lock)
 *
 * cam_scanner_start() OWNS the bring-up the coordinator deliberately does not: it
 * creates the camera pipeline (centered decode square), builds the overlay over the
 * active screen, and wires a coordinator whose injected present()/on_complete() drive
 * the overlay. The MicroPython consumer (eventually Python DecodeQR) then drains the
 * NEW ring + status cell on its own task and reports domain results back — see
 * docs/camera-pipeline-phase2-poll-contract.md.
 *
 * This header is binding-facing and SCALAR-ONLY by design: it pulls in NO engine,
 * LVGL, or k_quirc headers, so the MicroPython bindings' QSTR-scan include set (which
 * sees only this directory) stays clean — exactly the display_manager.h / dm_mem_stats
 * pattern. The scan_coordinator / pipeline / overlay headers live entirely in the .cpp.
 */
#ifndef CAMERA_SCANNER_H
#define CAMERA_SCANNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Neutral per-frame scan status — values MIRROR scan_coordinator's
 * scan_frame_status_t (and Python DecodeQRStatus) so report() and read_status()
 * share one vocabulary across the C↔Python boundary. The binding exposes these as
 * module constants (camera_scanner.FRAME_*).
 */
typedef enum {
    CAM_SCAN_FRAME_NONE   = 0,  /* no recent decode (engine NOTHING)   */
    CAM_SCAN_FRAME_NEW    = 1,  /* new part      (Python PART_COMPLETE) */
    CAM_SCAN_FRAME_REPEAT = 2,  /* already-seen  (Python PART_EXISTING) */
    CAM_SCAN_FRAME_MISS   = 3,  /* located, not decoded (engine MISS)  */
} cam_scan_frame_status_t;

/*
 * Coalesced status snapshot for the binding — a scalar projection of
 * scan_status_t. `corners` are intentionally OMITTED here (they require k_quirc
 * types this header must not pull in); has_corners is reserved and stays false
 * until the engine plumbs the MISS path (contract §7 tier 3). When that lands,
 * the corners array is added to the binding via a sibling accessor, leaving this
 * scalar struct and the attrtuple it feeds purely additive.
 */
typedef struct {
    int      latest;             /* cam_scan_frame_status_t of the most recent frame */
    uint32_t consecutive_misses; /* run of MISS frames; any non-MISS resets it       */
    uint32_t dropped_new;        /* monotonic NEW parts dropped on ring overflow      */
    bool     has_corners;        /* reserved; false until engine surfaces MISS corners */
} cam_scanner_status_t;

/*
 * Bring up the scan pipeline over the active LVGL screen. Idempotent: a second
 * call while running is a no-op success. Returns NULL on success, or a short
 * static error string (no camera on this board, pipeline/overlay/coordinator
 * create failure) that the binding raises. Must be called from the MicroPython
 * (consumer) task — it takes the esp_lvgl_port lock for the overlay build.
 *
 * focus_assist selects a DIFFERENT session: instead of the scan overlay +
 * coordinator, it brings up the camera preview with an on-screen software
 * focus meter (Laplacian sharpness; quirc skipped so the loop runs at the
 * camera rate) so a user can dial the fixed lens to the sharp plane. The C
 * side renders the meter entirely — no NEW-ring/report()/poll contract applies
 * in this mode (poll_new/read_status/report are inert). Pass false for the
 * normal QR scan session.
 */
const char *cam_scanner_start(bool focus_assist);

/* Tear down coordinator + overlay + pipeline. Idempotent. Takes the LVGL lock. */
void cam_scanner_stop(void);

/* True while a scan session is live (between start and stop). */
bool cam_scanner_is_running(void);

/*
 * CONSUMER task: drain one NEW payload from the ring. Returns true and fills
 * the out params on success; false when the ring is empty. The bytes are valid
 * ONLY until the next call — the binding copies them into a Python bytes at once.
 */
bool cam_scanner_poll_new(const uint8_t **payload, size_t *len);

/* CONSUMER task: snapshot the coalesced status + counters. */
void cam_scanner_read_status(cam_scanner_status_t *out);

/*
 * Diagnostic miss-frame metadata -- a scalar projection of the engine's
 * cam_pipeline_qr_miss_meta_t (kept out of this scalar-only header). See
 * cam_scanner_poll_miss_frame().
 */
typedef struct {
    uint32_t seq;          /* monotonic capture index                        */
    int64_t  timestamp_us; /* capture time                                   */
    int      quirc_err;    /* k_quirc error of the rejected code (-1 if none) */
    float    side_px;      /* measured QR side length in the crop (px)        */
    float    sharpness;    /* Laplacian edge energy of the crop (focus proxy) */
    uint8_t  luma_mean;    /* mean crop luminance 0..255                      */
    uint32_t width;        /* grayscale crop dims (payload is width*height B) */
    uint32_t height;
} cam_scanner_miss_meta_t;

/*
 * CONSUMER task: drain the latest sampled MISS frame (diagnostic; only produces
 * frames during a scan on a CONFIG_CAM_PIPELINE_QR_DEBUG build). Returns true and
 * fills *payload (grayscale, width*height bytes, valid ONLY until the next call)
 * + *len + *meta when a new sampled miss is available; false otherwise.
 */
bool cam_scanner_poll_miss_frame(const uint8_t **payload, size_t *len,
                                 cam_scanner_miss_meta_t *meta);

/*
 * CONSUMER task: report the domain result of a frame back to the coordinator,
 * which dedups on (status, percent) and drives present() -> the overlay.
 * `status` is a cam_scan_frame_status_t value; `percent` is 0..100.
 */
void cam_scanner_report(int status, int percent);

/* CONSUMER task: terminal completion -> on_complete() once. Idempotent. */
void cam_scanner_report_complete(void);

/* ── Instrumentation-run knobs + telemetry (scalar mirror of the engine's
 * cam_pipeline_qr_instr_t; see docs/instrumentation-run-spec.md). All ints so
 * this header stays QSTR-scan-clean. Defaults (cam_scanner_instr_defaults) =
 * deployed behavior. ── */
typedef struct {
    int num_decoders;       /* 0 = board default; 1/2 = override             */
    int sweep_cap;          /* 0 = effort default; >0 numeric probe budget   */
    int ladder_select;      /* 0 stock; 1 additive-deep                      */
    int blend_gate_permille;/* 0 = shadow                                    */
    int gate_dedup;         /* bool: PENDING-dedup                           */
    int debounce_ms;        /* stage-0 stability debounce; 0 = off           */
    int seed_override;      /* bool: pin the sweep seed ...                  */
    int seed_offset;        /*   ... at this offset ...                      */
    int lock_freeze;        /*   ... and freeze the lock                     */
    int fixed_threshold;    /* bool: legacy fixed-threshold decode           */
    int effort_thorough;    /* bool: THOROUGH effort (static-media slots)    */
    int hash_gate;          /* bool: content-change gate enable (default 1)  */
    int instr_log;          /* bool: CSV rows + captures + extended stats    */
    int capture_nothing;    /* bool: sample NOTHING frames                   */
    int burst;              /* bool: capture every dispatched frame          */
} cam_scanner_instr_opts_t;

void cam_scanner_instr_defaults(cam_scanner_instr_opts_t *out);

/* Stage knobs for the NEXT cam_scanner_start() (consumed once; NULL clears). */
void cam_scanner_set_instr(const cam_scanner_instr_opts_t *opts);

/* CONSUMER task: drain complete CSV rows (which: 0 = decode CSV, 1 = gate
 * CSV). Returns bytes copied into buf (whole newline-terminated rows only;
 * 0 = nothing pending / telemetry off). */
size_t cam_scanner_instr_poll_csv(int which, char *buf, size_t buf_len);

/* CONSUMER task: latest formatted 2 s stats block for the run.log mirror.
 * True when a block newer than *last_seq was copied (updates *last_seq). */
bool cam_scanner_instr_poll_stats(char *buf, size_t buf_len, uint32_t *last_seq);

/* Scalar mirror of the engine capture meta (classes: 0 miss, 1 blend-flagged,
 * 2 decoded sample, 3 nothing, 4 burst). */
typedef struct {
    int      cls;
    uint32_t seq;
    uint32_t dispatch_seq;
    int64_t  ts_us;
    uint32_t width, height;
    int      decoder_id;
    int      outcome;      /* 0 nothing, 1 miss, 2 decoded                  */
    int      blend_score;  /* -1 when not computed                          */
    float    side_px;
    float    sharpness;
    int      luma;
} cam_scanner_capture_meta_t;

/* CONSUMER task: drain the oldest pending capture (grayscale crop; buffer
 * valid ONLY until the next call). */
bool cam_scanner_instr_poll_capture(const uint8_t **payload, size_t *len,
                                    cam_scanner_capture_meta_t *meta);

/* Live producer counters (drop audit + fragment accounting). */
typedef struct {
    uint32_t decode_rows, decode_drops;
    uint32_t gate_rows, gate_drops;
    uint32_t captures, capture_drops;
    uint32_t frag_arrived, frag_dispatched, frag_expired;
} cam_scanner_instr_counters_t;

void cam_scanner_instr_counters(cam_scanner_instr_counters_t *out);

/* Cold-start the adaptive-threshold lock on the live decoders (see
 * k_quirc_reset_lock). Called between BBQR-bench completion trials so each is an
 * independent cold-start sample. Safe while scanning; no-op with no camera. */
void cam_scanner_reset_lock(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_SCANNER_H */
