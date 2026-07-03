# ESP32-P4 Camera + Display Performance Roadblocks — Cited Research Synthesis

Date: 2026-06-27. This is a research deliverable (read-only); no code changes proposed.

Scope: Waveshare ESP32-P4 (32MB PSRAM) running OV5647 MIPI-CSI -> esp_video/V4L2 + ISP -> RGB565, displayed on ST7701 MIPI-DSI DPI 480x800 via esp_lcd + esp_lvgl_port + LVGL v9, plus a QR decoder consuming frames.

---

## Hardware facts grounding the analysis (from ESP32-P4 v1.3 datasheet + TRM v1.3)

- PSRAM: in-package, **16-bit ("HEX"/16-line) DDR bus, 200 MHz max** (datasheet S4.1.3.1). Datasheet's own peak formula reads "16 x 2 x 250 MHz = 8 Gbit/s" — an internal inconsistency (table says 200 MHz, worked example says 250 MHz). Practical raw peak ~6.4 Gbit/s (0.8 GB/s) at 200 MHz, up to ~8 Gbit/s (1.0 GB/s) at 250 MHz. Source: https://documentation.espressif.com/esp32-p4-chip-revision-v1.3_datasheet_en.html
- Single cached CPU memcpy out of PSRAM measured ~185.7 MB/s (vs ~775 MB/s SRAM->SRAM). Source: https://github.com/espressif/developer-portal/discussions/353 . This is a single-stream cache-limited figure, NOT the bus ceiling; DMA masters burst more efficiently.
- Internal SRAM: **768 KB HP L2MEM** (+32 KB LP +8 KB TCM). Part of L2MEM doubles as cache when PSRAM enabled. Cannot hold a full hi-res framebuffer.
- CPU: dual-core RISC-V HP (360 MHz default on v1.3, 400 MHz on v3.x silicon) + LP core.
- PPA: **exactly ONE SRM (scale-rotate-mirror) engine + ONE BLEND engine**, fed by a **single 2D-DMA shared with the JPEG codec**. TRM v1.3 Ch.35 ("PPA consists of two functional modules: SRM and BLEND"). Rotate is 90/180/270 only. SRM does YUV<->RGB CSC inline. Source: https://documentation.espressif.com/esp32-p4-chip-revision-v1.3_technical_reference_manual_en.pdf
- MIPI-CSI and MIPI-DSI: each 2 data lanes x 1.5 Gbps = 3.0 Gbps aggregate, D-PHY v1.1. ISP max 1920x1080.
- 480x800 RGB565 @60Hz scanout = 480*800*2*60 = **46.08 MB/s** (~4.6% of PSRAM peak, ~12% of DSI link). Display scanout is NOT the bottleneck at this resolution.

---

## Roadblock 1: PSRAM bandwidth as dominant bottleneck — CONFIRMED (official)

- **Espressif states it verbatim** in the PPA driver guide: "the PPA performance highly relies on the PSRAM bandwidth if the pictures are located in the PSRAM section. When there are quite a few peripherals reading and writing to the PSRAM at the same time, the performance of PPA operation will be greatly reduced." Source: https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/ppa.html (ESP-IDF v6.0.2). Confidence HIGH, official.
- LCD FAQ: display "PCLK settings is constrained by the bandwidth of the PSRAM." Source: https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/lcd.html . Official.
- Field report: SPI camera + RGB LCD together on P4 -> image shifts until PCLK dropped 26->10 MHz -> ~6 fps; attributed to DMA contention. https://github.com/espressif/esp-idf/issues/17967 . Medium (different bus combo than pure MIPI).
- esphome P4 DSI measurements localize cost to full-frame buffer copy + cache-sync, NOT the DSI peripheral (avg flush 51us with zero-copy + cache sync, ~10.6x faster than safe-staging). https://github.com/esphome/esphome/issues/16873 . High (concrete numbers).
- GAP: no public single GB/s figure stated by Espressif; no DMA-arbitration/priority budget table; no benchmark isolating all-four-concurrent (CSI+ISP+PPA+DSI) with a measured aggregate.

## Roadblock 2: PPA single-SRM serialization — CONFIRMED by architecture (not by a verbatim "serializes" sentence)

- One SRM + one BLEND on a single 2D-DMA shared with JPEG codec (TRM v1.3). Using PPA for camera scale/rotate AND display rotation funnels both onto the one SRM datapath -> serialized + contends for 2D-DMA + PSRAM.
- Driver provides software thread-safety that serializes API calls across clients onto the single engines. https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/ppa.html
- SRM **cannot rotate in-place** — requires separate src/dst buffers (two large frames in PSRAM, read+write). Feeds back into roadblock 1. (BLEND can write in-place.)
- Community corroboration: LVGL "PPA queue depth overflow" messages + tearing on P4. https://github.com/lvgl/lvgl/issues/9046 (LVGL 9.4.0-dev). Medium.
- WORKAROUNDS (Espressif BSP engineer @espzav, https://github.com/espressif/esp-bsp/issues/400):
  - SW rotation measured FASTER than PPA for partial-screen flushes (PPA needs extra full-size PSRAM buffer + runs every flush). PPA only wins for WHOLE-buffer rotation.
  - Let the camera **ISP** do YUV->RGB (esp_cam_ctlr_format_conversion), so you don't burn an SRM pass on CSC. SRM also does CSC inline if you do use it -> rotate+convert in one pass.
  - "Rotate once not twice."
  - NO plain-DMA rotate alternative: async-memcpy/DW-GDMA are copy-only, cannot rotate. Rotation = PPA SRM or CPU only.
- GAP: no published PPA throughput numbers (Mpixels/s, latency). esp32.com perf thread is bot-gated.

## Roadblock 3: MIPI-DSI DPI panel rotation — CONFIRMED no HW rotation; SW/PPA framebuffer rotation required

- `esp_lcd_panel_swap_xy()` on a DSI panel returns "swap_xy is not supported by this panel." https://github.com/espressif/esp-idf/issues/14639 . Official, HIGH. DPI has no MADCTL-style hardware rotation for continuous scanout.
- No "rotate during scanout" path exists on P4 — DSI reads the framebuffer in normal raster order; every rotation is a PPA/CPU framebuffer transform BEFORE scanout. NOT FINDABLE otherwise — treat any claim of rotated-scanout as speculative.
- esp_lvgl_port LATEST = **v2.8.0**. Rotation timeline: v2.3.0 SW rotation; v2.4.0 direct/full-refresh for MIPI-DSI; **v2.6.0 added PPA rotation for ESP32-P4**; v2.7.1 fixed PPA rotation for IDF6. https://components.espressif.com/components/espressif/esp_lvgl_port/versions/2.8.0~1/changelog . HIGH.
- THE KNOWN CONFLICT (confirmed both sides): esp_lvgl_port README — "When using software rotation, you cannot use neither direct_mode nor full_refresh in the driver." So sw_rotate is mutually exclusive with avoid_tearing. LVGL frames it as: DIRECT mode renders small changed areas straight into the framebuffer so they can't be rotated later. https://components.espressif.com/components/espressif/esp_lvgl_port/versions/2.6.2 + https://lvgl.io/docs/open/main-modules/display/rotation . HIGH.
- Known crash: esp_lvgl_port 2.6.2 + LVGL 9.4.0 on P4/ILI9881C, 90/270 sw_rotate -> "Load access fault" in dpi_panel_draw_bitmap. https://github.com/espressif/esp-bsp/issues/665 . May be fixed in 2.7.x/2.8.x (changelog shows ongoing PPA-rotation fixes).
- THE RESOLUTION (Espressif's modern blessed path): **esp_lvgl_adapter** (esp-iot-solution) supports rotation + anti-tearing together. **TRIPLE_PARTIAL mode = "90/270 rotation, High-res smooth UI"**, 3 framebuffers, "Rotation uses PPA when possible; otherwise CPU copy." TEAR_AVOID_MODE_NONE forbids rotation. https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/tools/esp_lvgl_adapter.html . Official, HIGH. (Note a documented P4 freeze fix patch sets PPA.sr_byte_order.sr_macro_bk_ro_bypass=1 for IDF v6.0.)

## Roadblock 4: Official P4 camera->display reference designs + achieved performance

- esp-idf examples/peripherals/camera/mipi_isp_dsi: CSI->DSI passthrough, RAW8, sensor configs 800x1280@50 / 800x800@50 / 800x640@50, OV5647/SC2336. **No LVGL/UI overlay** — raw capture-to-display. https://github.com/espressif/esp-idf/tree/master/examples/peripherals/camera/mipi_isp_dsi . "50fps" = sensor input rate, not measured on-screen fps.
- esp-iot-solution examples/camera/video_lcd_display: SC2336 1280x720@30 -> ISP -> RGB565 -> draw_bitmap to 1024x600 DSI. Source confirmed NO lv_* and NO ppa_* calls. https://raw.githubusercontent.com/espressif/esp-iot-solution/master/examples/camera/video_lcd_display/README.md (ESP-IDF v5.4+).
- esp-video suite: camera capture/encode/stream; none drive a local display with overlay. Sensor rates 720p@30 etc.
- **esp-who WhoRecognitionAppLCD (P4): DOES overlay LVGL on live feed** (face bounding boxes, enroll/recognize UI). Detection input QVGA 320x240. SW rotation HW-accelerated via PPA. Camera+model async "to achieve higher fps." https://github.com/espressif/esp-who . BUT no published fps for P4.
- ESP32-P4-EYE factory demo: overlays UI (photo/video/album/AI YOLOv11-nano) — but display is **240x240 SPI, NOT DSI**. https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4-eye/user_guide.html
- M5Stack Tab5 (P4, 1280x720 DSI, SC2356): full-screen camera preview measured **~1-2 fps**; UI coexists; used PPA to rotate 180; resolution-tuning to speed up "failed." https://www.cnx-software.com/2025/05/14/... + .../2025/05/18/... . Community measurement, the single most concrete DSI-preview-with-UI number found.
- LVGL #10260 tracks PPA "live camera canvas" (camera as lv_canvas + widgets) — but fps is a TODO/success-criterion, NOT a published result; notes PPA image BLENDING gives "no significant performance gains" (DMA-2D bandwidth bound). https://github.com/lvgl/lvgl/issues/10260

## Roadblock 5: Frame-rate expectations for ~480x480 RGB565 live preview

- Realistic: **~8-15 fps from a straightforward/community implementation**; **1-2 fps if blitting a large full-screen frame with SW scaling** (Tab5); **25-30 fps only with a zero-copy, hardware-PPA, DMA path + modest buffers** (inferred from headroom, NOT directly measured anywhere clean).
- Sensor is NOT the limiter: OV5647 Espressif driver runs 800x640 RAW8 @50fps (no 480x480 mode exists in the driver — smallest is 800x640; everyone captures bigger then scales). Other sensors hit 100fps@640x480. https://github.com/espressif/esp-video-components/blob/master/esp_cam_sensor/sensors/ov5647/ov5647.c
- DSI link NOT the limiter: 3 Gbps, 1280x800@60 proven; 480x480 RGB565 has ample headroom.
- DOMINANT bottleneck: **PSRAM bandwidth contention + full-frame copy / RAW->RGB565 conversion / cache-sync stage**, which scales with output FRAME SIZE. Confirmed by PPA docs (official) + esphome measurements (localized to buffer handling, not DSI).
- Espressif states camera driver params "not optimal... significant room for optimization." https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/solution-introduction/camera/dvp-mipi-csi-camera-solution.html
- CONFLICT reconciliation: high numbers ("50fps capture", "100fps sensor", "15fps") are sensor-output / small-window MJPEG / H.264-RTSP-streaming respectively — NOT full-screen DSI preview. The recurring "15 fps on P4" is the H.264->RTSP network pipeline, NOT local DSI preview. The P4-EYE "15fps@480p / 5fps@720p" is SD-card-write-bound recording, a different bottleneck.

---

## Cross-cutting conclusions for the SeedSigner use case

1. PSRAM bandwidth is the shared ceiling. Every engine (CSI write, ISP, PPA src+dst, DSI scanout, QR decoder read) draws from one ~0.8-1.0 GB/s raw pool. This is officially documented, not speculation.
2. Don't rotate twice on the PPA. Camera-frame transform AND display rotation both on the one SRM engine serializes. Let the ISP do CSC; rotate once.
3. For landscape UI on portrait DSI: the modern Espressif-blessed path that keeps anti-tearing is esp_lvgl_adapter TRIPLE_PARTIAL (3 framebuffers, PPA). esp_lvgl_port's sw_rotate works but disables avoid_tearing/direct_mode/full_refresh.
4. Frame-size discipline is the single biggest lever. Keep the live camera region small/scaled (QVGA-class, like esp-who) rather than compositing UI over a full-screen 720p+ feed. Full-screen full-res live preview with LVGL overlay is the unsolved hard case on P4.
5. Realistic target: ~8-15 fps for a modest preview is normal; 30fps needs zero-copy + PPA + small buffers + minimizing simultaneous PSRAM clients. No clean measured 30fps full-screen DSI preview was found in public sources.

## Honest gaps (NOT findable)
- No public single GB/s PSRAM bandwidth figure or DMA-arbitration/priority table from Espressif.
- No benchmark isolating CSI+ISP+PPA+DSI all concurrent with a measured aggregate.
- No published PPA throughput (Mpixels/s) numbers.
- No official on-screen fps for any P4 camera->DSI preview example (only sensor-input rates documented).
- No clean measured 30fps full-screen DSI live-preview datapoint.
- PPA engine count (1 SRM + 1 BLEND) is in the TRM but not stated verbatim in the public datasheet/API docs.
