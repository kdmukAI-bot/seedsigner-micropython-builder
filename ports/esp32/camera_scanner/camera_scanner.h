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
 */
const char *cam_scanner_start(void);

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
 * CONSUMER task: report the domain result of a frame back to the coordinator,
 * which dedups on (status, percent) and drives present() -> the overlay.
 * `status` is a cam_scan_frame_status_t value; `percent` is 0..100.
 */
void cam_scanner_report(int status, int percent);

/* CONSUMER task: terminal completion -> on_complete() once. Idempotent. */
void cam_scanner_report_complete(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_SCANNER_H */
