# Research: LVGL UI overlay on live camera preview (ESP32-P4 / MIPI-DSI)

Date: 2026-06-27. This is a research deliverable, not an implementation plan.
Target context: ESP32-P4, LVGL v9 via esp_lvgl_port v2.7.2, MIPI-DSI DPI panel 480x800,
video-mode continuous scanout, multi-FB, direct_mode + avoid_tearing. Camera pipeline
produces PPA-scaled RGB565 frames. Goal: live camera as full-screen background with LVGL
widgets composited on top at acceptable FPS, without forcing LVGL to recomposite/flush the
whole screen every camera frame.

## Bottom line up front
The strongest, most-repeated guidance across LVGL maintainers and a vendor (NXP) support
thread is: **do NOT push camera frames through an lv_canvas / lv_image when you want high
frame rate.** That path costs a per-frame CPU copy into the LVGL draw buffer plus a
full-area invalidate, which is exactly the "redraw/rotate the whole screen every frame" wall
described in the prompt. The recommended high-FPS pattern is **hardware-layer compositing**:
the camera DMAs directly into its own framebuffer/layer, LVGL renders only the UI into a
*separate transparent* layer, and the display controller blends them with no CPU copy.

The catch specific to ESP32-P4: its blending accelerator (PPA) is **memory-bandwidth bound on
DMA-2D**, so PPA-blend gives "no significant gains" per LVGL's own docs, and PPA acceleration
currently has open tearing/dirty-rect bugs on P4. So the cheapest composite is one the *DSI/
DPI scanout* does for free (overlapping framebuffers) rather than a per-frame PPA blend.

---

## Q1 — Canonical "video background + widget overlay" pattern

Claim: LVGL maintainer (kisvegabor) gives two sanctioned options.
- (A) **Two framebuffers + transparent screen**, hardware merges them: "Do you have 2 frame
  buffers (one for the video and another for the UI and you hardware merges them)? If so you
  need to enable LV_COLOR_SCREEN_TRANSP." (kisvegabor, 2020-07-09)
- (B) **Single buffer**: wrap the video memory in a fake lv_img_dsc_t inside an lv_img, and
  lv_obj_invalidate() on each new frame.
- HARD CONSTRAINT: "you can't write the same screen (layer) with LVGL and by hand too. The
  JPEG stream also should be managed by LVGL." (kisvegabor, 2021-03-02). I.e. if the camera
  writes directly into the same buffer LVGL renders into, they fight — they must be *different*
  layers/buffers.
Source: https://forum.lvgl.io/t/how-do-i-combine-a-lvgl-gui-and-a-live-video-stream/2653
Confidence: HIGH. Official maintainer guidance. Note LV_COLOR_SCREEN_TRANSP is v8-era naming;
v9 equivalent is a transparent display + screen-transparent color format (see Q3).

Claim: Pushing camera frames through lv_canvas/lv_img is the slow path.
- NXP thread (Vinos): canvas approach "introduced additional processing overhead in
  transferring camera frames to the Canvas, resulting in high CPU usage and choppy camera feed."
- esp-cam forum: even with lv_img + the JPEG decoder, realistic ceiling is "5-10fps"
  (embeddedt); user targeted 10-15 fps. Bottleneck = decode + copy, not the screen.
Sources:
  https://community.nxp.com/t5/i-MX-RT/How-to-smoothly-display-camera-frames-in-LVGL/m-p/1698144
  https://forum.lvgl.io/t/how-to-display-video-stream-from-esp-cam/4970
Confidence: HIGH (consistent across two independent threads + a vendor).

Verdict for high-FPS: **option (a) lv_canvas-as-background is NOT the recommended high-FPS
path.** Best = (c) bypass LVGL for the camera, LVGL only draws a transparent overlay that the
hardware blends. Maintainer-endorsed.

## Q2 — Avoiding full-screen redraw per camera frame
- If the camera lives on its own layer/FB, LVGL never sees it and only redraws UI dirty rects
  — the overlay stays partial/cheap. This is the entire point of the layer split.
- If you insist on a single LVGL screen with the camera as an lv_image child: any frame update
  = lv_obj_invalidate on a screen-sized object = full composite + flush; with direct_mode/
  full_refresh this is by definition a whole-screen redraw (LVGL docs: full_refresh "forces
  LVGL to always redraw the whole screen"; direct_mode renders into a screen-sized FB).
- LVGL v9 widget facts useful for the overlay: new widgets draw on top of older ones by
  default; lv_obj_move_foreground/background reorder; an lv_image can be a full-screen
  wallpaper with normal child widgets on top. layer_top is a screen-wide always-on-top layer
  good for status text / framing guides.
- v9 PPA *draw* acceleration helps fills/rotate but NOT blend (see Q3/Q5) — it does not make a
  per-frame full-screen camera composite cheap.
Sources:
  https://docs.lvgl.io/master/main-modules/display/refreshing.html
  https://forum.lvgl.io/t/what-does-full-refresh-do/11386
  https://lvgl.io/docs/open/widgets/image
  https://docs.lvgl.io/master/main-modules/display/setup.html
Confidence: HIGH for the mechanism; MEDIUM that there's no v9 trick to make a screen-sized
per-frame invalidate cheap (could not find one — the answer everyone gives is "split layers").

## Q3 — Multiple framebuffers / panel-layer compositing (the recommended path)
- NXP (EdwinHz, TechSupport) lays out the canonical 2-layer design on an i.MX-RT LCDIFv2 — same
  shape as P4 DPI: "Using another layer of LCDIFv2 as output buffer of camera directly, then
  LCDIFv2 h/w composites different layers on display without copy." Layer 0 = LVGL UI (white
  bg), Layer 1 = camera written directly by CSI, transparency via ARGB4444 +
  LCDIFV2_SetLayerBlendConfig. Camera shows smoothly; CPU bottleneck disappears.
  Source: https://community.nxp.com/t5/i-MX-RT/How-to-smoothly-display-camera-frames-in-LVGL/m-p/1698144
  Confidence: HIGH as a pattern; MEDIUM transfer to P4 (P4 has no LCDIFv2 — see caveat).
- LVGL OSD pattern, official: "Transparent screen ... could be used to create OSD menus where
  a video is played on a lower layer, and a menu is overlayed on an upper layer ... needs to be
  enabled with LV_COLOR_SCREEN_TRANSP." Slower SW color-mixing if LVGL does the blend.
  Source: https://docs.lvgl.io/master/main-modules/display/setup.html
- ESP32-P4 PPA *can* blend in hardware, BUT LVGL's own PPA page: "for image blending, even
  though it is operational, there are no significant gains, the initial cause ... is due to the
  DMA-2D memory bandwidth," and PPA "will not offer performance increase when using it in
  partial mode due to DMA2D memory bandwidth." PPA marked EXPERIMENTAL. Fills up to 9x,
  rotate ~40% saving — but blend ~0.
  Source: https://lvgl.io/docs/open/integration/chip_vendors/espressif/hardware_accelerator_ppa
  Confidence: HIGH (official LVGL doc).
- P4 IMPORTANT CAVEAT: P4's MIPI-DSI/DPI peripheral does NOT expose arbitrary alpha-blended
  hardware overlay *layers* the way NXP LCDIFv2 / STM32 LTDC do. P4 composites by having the
  DPI scan out one framebuffer; "layering" on P4 is done either by (i) PPA/2D-DMA blending into
  a single FB (bandwidth-bound, per above) or (ii) pointing the DPI at the camera FB and
  drawing UI into the *same* FB. I could NOT find an Espressif example doing free hardware
  multi-layer alpha overlay of camera-FB + UI-FB on P4 DSI. State this as a gap. The closest
  Espressif primitive is PPA blend (bandwidth-limited) or the 2D-DMA copy.
  Confidence: MEDIUM-HIGH (absence of evidence + P4 TRM/PPA framing; flag for hardware-kb check).

Practical P4 takeaway: the realistic "free" composite is **draw the UI directly into the same
framebuffer the camera fills** (UI is small dirty rects, camera fills the rest), OR accept a
PPA/2D-DMA blend and live with the bandwidth ceiling. A true zero-cost dual-layer alpha
overlay like LCDIFv2/LTDC is not a P4 hardware feature as far as public docs show.

## Q4 — ESP32-P4 viewfinder reference designs WITH UI
- **ESP-WHO (Espressif)** is the most on-point reference: supports ESP32-P4 Function EV Board,
  "Add lvgl support," runs camera + face detection, WhoRecognitionAppLCD "manages the camera
  frames, runs detection ... and renders the results on the display using LVGL," drawing
  bounding boxes on detected faces. Camera and model run asynchronously for higher FPS.
  Requires ESP-IDF v5.5.x (5.5.4). The PUBLIC docs/README do NOT state whether the camera frame
  itself goes through LVGL or is blitted directly with boxes drawn on top — must read example
  source (who_recognition_app_lcd) to confirm. This is the best lead for a concrete P4 overlay.
  Sources:
    https://github.com/espressif/esp-who
    https://developer.espressif.com/blog/2026/05/esp-who-get-started/
  Confidence: HIGH it exists + uses LVGL + P4 + draws overlays; MEDIUM on exact mechanism
  (need source read).
- **ESP-IDF camera examples** peripherals/camera/mipi_isp_dsi and dvp_isp_dsi: CSI camera ->
  ISP -> DSI panel, **no LVGL, no UI overlay** — frames written directly to panel. Good baseline
  for "camera straight to DSI FB," and the natural thing to add a UI layer onto. OV5647 ~50fps
  sensor-side. Source:
    https://github.com/espressif/esp-idf/tree/master/examples/peripherals/camera/mipi_isp_dsi
  Confidence: HIGH.
- **ESP-IDF PPA example** peripherals/ppa/ppa_dsi: demonstrates scale/rotate/mirror + **blend a
  semi-transparent layer + color-key + frame fill** to a DSI display — the literal primitive
  for compositing an overlay onto a camera FB via PPA. Source:
    https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/ppa.html
  Confidence: HIGH (it's the blend reference; subject to the bandwidth caveat in Q3/Q5).
- **M5Stack Tab5 (ESP32-P4, SC2356 2MP CSI, 1280x720 DSI)**: lots of LVGL demos (max ~62 FPS
  UI), MicroPython+LVGL, Arduino+LVGL basic demos, ESPP BSP. I found Tab5 *camera* demos and
  *LVGL* demos but did NOT find a single canonical Tab5 sample that shows live camera as LVGL
  background WITH widget overlay and a stated FPS. Treat as: building blocks exist, no turnkey
  combined reference located. Sources:
    https://esp-cpp.github.io/espp/m5stack_tab5.html
    https://www.hackster.io/vsupacha/exploring-m5stack-tab5-with-micropython-lvgl-0e2392
    https://github.com/nikthefix/M5Stack_Tab5_Arduino_Basic_LVGL_Demo
  Confidence: MEDIUM (could-not-find is a real result here).

## Q5 — Known LVGL+camera gotchas + the PSRAM/PPA bandwidth wall
- **PPA blend = PSRAM/DMA-2D bandwidth wall, confirmed by others.** LVGL docs (Q3) say blend
  gains ~0 due to DMA-2D bandwidth; partial mode no gain for the same reason. This is exactly
  the prompt's "PPA serialization / PSRAM bandwidth" hypothesis, and it's documented as a real
  ceiling, not speculation. Mitigation that others use: keep buffers in INTERNAL RAM and
  zero-copy. The esphome P4 perf work (800x800 RGB888, ESP-IDF 5.5.4, LVGL 9.5) got ~10.6x
  flush improvement (449.7ms -> 42.2ms; per-flush 551us -> 51us) by ZERO-COPY (internal RAM +
  DMA-aligned + explicit cache sync) and moving DSI completion to a dedicated core-0 task
  (async flush) so display sync is off the render hot path. "Placing the frame buffer into
  external PSRAM yields worse results."
  Sources:
    https://github.com/esphome/esphome/issues/16873
    https://lvgl.io/docs/open/integration/chip_vendors/espressif/hardware_accelerator_ppa
  Confidence: HIGH.
- **PPA acceleration causes tearing + stale "ghost" dirty rects on P4** (open bug):
  LVGL #9046, v9.4.0-dev (Oct 2025), P4 800x1280@60, ESP-IDF 5.3 — "PPA queue depth overflow",
  tearing, ghosting; suspected PPA sync/cache mgmt; assigned @uLipe, PR #9162 in flight. Also
  #7698 P4 LVGL 9.2.2 tearing of floating FPS indicator when no background element. Implication:
  enabling PPA for the overlay can itself introduce tearing on P4 right now.
  Sources:
    https://github.com/lvgl/lvgl/issues/9046
    https://github.com/lvgl/lvgl/issues/7698
  Confidence: HIGH (open issues).
- **Tearing vs PSRAM starvation (bounce buffer).** Community fix for RGB/DPI panels: a "bounce
  buffer" that ties up the CPU during refresh to stop simultaneous PSRAM access from starving
  the LCD peripheral. Relevant because the camera DMA + DPI scanout + LVGL all contend for
  PSRAM. Source: (search synthesis of LVGL/ESP RGB-panel tearing threads)
    https://forum.lvgl.io/t/frame-buffers-and-tearing-effect/17555
  Confidence: MEDIUM (well-known technique; not P4-camera-specific).
- **Buffer ownership / lock contention.** LVGL is not thread-safe: all lv_ calls must hold the
  LVGL lock (esp_lvgl_port gives lvgl_port_lock/unlock); on_vsync runs in ISR context and must
  be ISR-safe. With a separate camera task you must not touch LVGL objects from it, and you
  must not let the camera write the FB LVGL is mid-render on (echoes kisvegabor's "can't write
  the same layer by hand and by LVGL"). Source:
    https://github.com/lvgl/lvgl/issues/9046 (lock/ISR notes) + setup docs above.
  Confidence: HIGH.
- **Direct-write vs LVGL-flush incompatibility on P4 DSI (cautionary).** esphome #10746: on
  M5Stack Tab5, direct lambda framebuffer drawing works but enabling LVGL goes black —
  suspected missing flush / wrong DSI-video-mode sync. Lesson: mixing direct FB writes (camera)
  and LVGL flush on P4 DSI video mode needs careful flush/swap coordination or one silently
  wins. Source: https://github.com/esphome/esphome/issues/10746
  Confidence: MEDIUM (open, root cause inferred).
- **Resolution / frame-skip mitigations** others use: lower preview resolution, PPA-downscale
  the preview window (not full sensor res), prefer RGB565 over RGB888 to halve bandwidth, skip
  frames. P4 practical numbers seen: 720p ~20 fps camera; UVC-in-canvas only allows shrinking
  resolution to keep up. Sources:
    https://docs.espressif.com/projects/esp-faq/en/latest/application-solution/camera-application.html
    https://github.com/espzav/UVC-Camera-and-MSC-LVGL-Example
  Confidence: MEDIUM-HIGH.

## What I could NOT find (be explicit)
1. A turnkey ESP32-P4 example showing **live camera as LVGL background + widget overlay with a
   stated FPS** (M5Stack Tab5 or otherwise). Building blocks yes; combined reference no.
2. Public confirmation that **P4 DSI exposes free hardware alpha overlay layers** (LCDIFv2/LTDC
   style). Evidence points to PPA/2D-DMA blend (bandwidth-bound) or single-FB shared draw
   instead. Recommend verifying against P4 TRM / hardware-kb.
3. Exact ESP-WHO rendering mechanism on P4 (camera-through-LVGL vs direct-blit + boxes) — needs
   a read of who_recognition_app_lcd source.
4. esp32.com forum threads specifically quantifying the camera+display PSRAM bandwidth wall on
   P4 (the bandwidth claim is sourced from LVGL docs + esphome issue, not a forum benchmark).

## Recommended pattern for this project (synthesis)
1. Camera pipeline writes PPA-scaled RGB565 frames into its OWN framebuffer; do NOT route through
   lv_canvas/lv_image (avoids per-frame CPU copy + full invalidate — Q1/Q2).
2. Two viable P4 composites:
   a. Point DPI at the camera FB and have LVGL draw UI as small dirty-rect overlays into the
      SAME FB region (cheapest; UI invalidates stay partial). Coordinate flush/swap with DSI
      video mode (heed #10746). No alpha-layer hardware needed.
   b. If you need true alpha over video, PPA-blend a transparent UI layer onto the camera FB —
      but budget for the DMA-2D bandwidth ceiling (Q3/Q5) and current PPA tearing bugs (#9046).
3. Keep the actively-scanned buffers in INTERNAL RAM, DMA-aligned, cache-synced; async/off-core
   flush; RGB565 not RGB888 — mirrors the esphome 10.6x win (Q5).
4. Treat ESP-WHO (P4 EV board, LVGL, async camera+detect, bounding-box overlay) as the closest
   working reference; read its LCD app source to copy the overlay approach (Q4).
5. Mitigate the bandwidth wall the way others do: lower preview resolution / partial preview
   window / frame-skip; never PSRAM for the live buffers.
