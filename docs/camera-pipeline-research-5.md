# Research: Rotating a landscape UI on a portrait-native ESP32-P4 MIPI-DSI DPI panel

Question: How to render LANDSCAPE on a PORTRAIT-native DSI (DPI / video-mode, continuous-scanout) panel efficiently on ESP32-P4, given that DPI panels can't use MADCTL/controller hardware rotation the way SPI/command-mode panels can.

## Findings (discrete, cited)

### F1 — Hardware swap_xy/mirror is NOT supported by the MIPI-DSI DPI driver
- CLAIM: Calling `esp_lcd_panel_swap_xy()` on a MIPI-DSI panel (ILI9881C on ESP32-P4) returns an explicit error: "swap_xy is not supported by this panel"; the DSI/DPI driver does not implement hardware rotation.
- SOURCE: https://github.com/espressif/esp-idf/issues/14639 (IDFGH-13779)
- VERSION/DATE: ESP-IDF (master, ~late 2024). Issue status internally "Done".
- CONFIDENCE: high
- TYPE: official Espressif issue tracker (user report + Espressif assignment; error string is from the IDF driver itself)

### F2 — Software rotation is the required fallback for DSI/DPI
- CLAIM: With no hardware rotation, `lv_draw_sw_rotate` is used to rotate the buffer in the flush callback; software rotation is the recommended path for DSI panels lacking swap_xy.
- SOURCE: https://github.com/espressif/esp-idf/issues/14639 ; corroborated https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/lcd.html
- CONFIDENCE: high
- TYPE: official Espressif

### F3 — esp_lvgl_port LATEST version is 2.8.0 (2.8.0~1 on registry)
- CLAIM: Latest published esp_lvgl_port is v2.8.0 (shown as 2.8.0~1). v2.8.0 added "Supported RGB/MIPI-DSI interfaces for chips by SOC_*", ESP32-P4 RGB example, RGB565 swapped color, LCD_RGB_ISR_IRAM_SAFE.
- SOURCE: https://components.espressif.com/components/espressif/esp_lvgl_port/versions/2.8.0~1/changelog
- CONFIDENCE: high
- TYPE: official Espressif Component Registry changelog

### F4 — esp_lvgl_port HAS a sw_rotate option and uses PPA on ESP32-P4
- CLAIM: Rotation is selected via `lvgl_port_display_cfg_t.flags.sw_rotate` (true = software rotation, false = hardware). "Software rotation uses PPA if available on the chip (e.g. ESP32P4)." "Software rotation consumes more RAM."
- SOURCE: https://components.espressif.com/components/espressif/esp_lvgl_port/versions/2.6.2 (README)
- CONFIDENCE: high
- TYPE: official Espressif README

### F5 — esp_lvgl_port rotation feature timeline (changelog, verbatim entries)
- CLAIM: SW rotation and DSI support were added incrementally:
  - v2.0.0 — "Added support for LVGL9" + "Added support for MIPI-DSI display"
  - v2.2.0 — "Added support for direct mode and full refresh mode"; breaking: removed MIPI-DSI from display config struct
  - v2.3.0 — "Added support for SW rotation in LVGL9"
  - v2.4.0 — "Added support for direct mode and full refresh mode in MIPI-DSI" + optimized avoid-tear
  - v2.4.2 — "Fixed SW rotation in LVGL9.2"
  - v2.4.4 — "allow byte swapping with SW rotation"
  - v2.6.0 — "Added support for PPA rotation in LVGL9 (available for ESP32-P4)"  ← PPA-accelerated rotation lands here
  - v2.7.1 — "Fixed PPA rotation for IDF6"
- SOURCE: https://components.espressif.com/components/espressif/esp_lvgl_port/versions/2.8.0~1/changelog
- CONFIDENCE: high
- TYPE: official Espressif changelog

### F6 — THE KNOWN CONFLICT: sw_rotate cannot combine with direct_mode or full_refresh (the anti-tearing modes)
- CLAIM (Espressif): "When using software rotation, you cannot use neither `direct_mode` nor `full_refresh` in the driver."
- SOURCE: https://components.espressif.com/components/espressif/esp_lvgl_port/versions/2.6.2 (README)
- CLAIM (LVGL official, root cause): In `LV_DISPLAY_RENDER_MODE_DIRECT` "the small changed areas are rendered directly in the frame buffer so they cannot be rotated later. Therefore in direct mode only the whole frame buffer can be rotated." FULL mode works only "if the buffer(s) being rendered to are different than the buffer(s) being rotated to in the flush callback and the buffers being rendered to do not have a stride requirement."
- SOURCE: https://lvgl.io/docs/open/main-modules/display/rotation (LVGL master/9.x)
- CONFIDENCE: high
- TYPE: official Espressif + official LVGL
- NUANCE: LVGL frames it as a constraint, not a flat prohibition — FULL mode can rotate with a separate destination buffer. esp_lvgl_port's own driver, however, hard-blocks the combination (it returns early / rejects it).

### F7 — Practical consequence: avoid_tearing must be OFF when sw_rotate is on (esp_lvgl_port)
- CLAIM: With esp_lvgl_port, "The `avoid_tearing` variable must be set to false when `sw_rotate` is active. Otherwise, conflicts will arise in buffer management." Confirmed Waveshare P4 10.1" recipe: `lv_disp_set_rotation(disp, LV_DISP_ROT_90)`, `sw_rotate = true`, double buffer in PSRAM, LVGL 8.4.*, esp_lvgl_port ^2 — "costs some performance but significantly more flexible than hardware rotation."
- SOURCE: https://www.haraldkreuzer.net/en/news/waveshare-esp32-p4-module-dev-kit-c-compact-development-board-101-inch-dsi-display
- CONFIDENCE: high (blog) — matches official docs
- TYPE: community blog, corroborates official

### F8 — Known bug: esp_lvgl_port 90/270 sw_rotate can crash on P4 DSI
- CLAIM: esp_lvgl_port v2.6.2 + LVGL 9.4.0 on ESP32-P4 / ILI9881C 800x1280: 90/270 software rotation triggers "Load access fault" panic in `dpi_panel_draw_bitmap`. Issue (BSP-732) was awaiting triage when fetched.
- SOURCE: https://github.com/espressif/esp-bsp/issues/665
- CONFIDENCE: high (that the bug was reported); medium on current status
- TYPE: official Espressif issue tracker (community report)

### F9 — Espressif's RECOMMENDED modern path: esp_lvgl_adapter (esp-iot-solution), which DOES combine rotation + anti-tearing
- CLAIM: The newer `esp_lvgl_adapter` component supports rotation simultaneously with tear-avoidance on RGB/MIPI-DSI. Per official docs: with `TEAR_AVOID_MODE_NONE` rotation is rejected, but `DOUBLE_FULL`, `TRIPLE_FULL`, `DOUBLE_DIRECT`, and `TRIPLE_PARTIAL` all support rotation. `TRIPLE_PARTIAL` is explicitly recommended for "90°/270° rotation, High-res smooth UI". Multiple sources say to use it "for higher frame rates, better rotation performance, and improved anti-tearing."
- SOURCE: https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/tools/esp_lvgl_adapter.html
- CONFIDENCE: high
- TYPE: official Espressif (esp-iot-solution)
- SIGNIFICANCE: This is the resolution to the F6/F7 conflict — esp_lvgl_adapter is purpose-built to do rotation AND anti-tearing together, which esp_lvgl_port's sw_rotate path cannot.

### F10 — HARDWARE rotation path on P4 exists: PPA rotates the framebuffer (not scanout-order rotation)
- CLAIM: ESP32-P4 has a PPA (Pixel Processing Accelerator) + 2D-DMA. Rotation is done by PPA-rotating the rendered buffer into the scanout framebuffer ("Rotation uses PPA when possible; otherwise CPU copy with cache-friendly blocks"). The DSI/DPI peripheral itself reads the framebuffer in normal raster order — there is no documented "rotate during scanout" / rotated-read mode; rotation is a pre-scanout buffer transform. With PPA, set `LV_DRAW_SW_DRAW_UNIT_CNT == 1`.
- SOURCE: https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/tools/esp_lvgl_adapter.html ; PPA capability https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/ppa.html (referenced from esp_lvgl_port README)
- CONFIDENCE: high (PPA exists, rotates buffer); high (no scanout-order rotation found — NOT findable = does not exist publicly)
- TYPE: official Espressif

### F11 — esp_lvgl_adapter P4 freeze bug + fix when rotation + TRIPLE_PARTIAL + PPA
- CLAIM: On ESP32-P4, `TRIPLE_PARTIAL` + rotation can freeze the display due to PPA; fix is patch `0001-bugfix-lcd-Fixed-PPA-freeze.patch` (sets `PPA.sr_byte_order.sr_macro_bk_ro_bypass = 1;`), targeting ESP-IDF tags/v6.0.
- SOURCE: https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/tools/esp_lvgl_adapter.html
- CONFIDENCE: high
- TYPE: official Espressif

### F12 — Frame-buffer count rule for rotation
- CLAIM: 90°/270° rotation (or any triple-buffer mode) needs 3 frame buffers; double-buffer modes need 2; single needs 1. API: `esp_lv_adapter_get_required_frame_buffer_count(tear_mode, rotation)`.
- SOURCE: https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/tools/esp_lvgl_adapter.html
- CONFIDENCE: high
- TYPE: official Espressif

## Conflicts / speculation flags
- F6 nuance: Espressif README says sw_rotate "cannot" use direct_mode/full_refresh (hard block in their driver); LVGL docs say FULL mode *can* rotate given a separate destination buffer + no stride requirement. Not contradictory — esp_lvgl_port simply doesn't implement the FULL+rotate case; esp_lvgl_adapter does (its FULL/PARTIAL+rotation modes).
- "Rotate during DSI scanout" (rotated framebuffer read order): NOT FOUND in any official source. The P4 DSI/DPI peripheral reads linearly; all rotation is a PPA/CPU pre-scanout buffer transform. Treat any claim of true scanout-order rotation as unsupported/speculative.
- F8 crash is real but version-specific (2.6.2 / LVGL 9.4.0); may be fixed in 2.7.x/2.8.x (changelog shows ongoing PPA-rotation fixes: 2.7.1 "Fixed PPA rotation for IDF6").

## BOTTOM LINE (synthesis)
On ESP32-P4 a MIPI-DSI DPI (video-mode) panel genuinely cannot be rotated by the controller — `esp_lcd_panel_swap_xy()` returns "swap_xy is not supported by this panel," so landscape-on-portrait must be done by rotating the framebuffer in software/PPA before scanout; there is no rotated-scanout read mode. `esp_lvgl_port` (latest v2.8.0) supports this via `.flags.sw_rotate = true`, and since v2.6.0 it PPA-accelerates the rotation on P4 — but its sw_rotate path is mutually exclusive with the anti-tearing modes (`direct_mode`/`full_refresh`/`avoid_tearing`), so the simple recipe (lv_disp_set_rotation + sw_rotate, avoid_tearing off, ≥3 buffers in PSRAM) trades tear-free output for rotation and can still hit a 90/270 crash on some versions. The clean, Espressif-blessed way to get rotation AND anti-tearing together is the newer `esp_lvgl_adapter` (esp-iot-solution), whose `TRIPLE_PARTIAL` mode is explicitly built for "90°/270° rotation, high-res smooth UI" using PPA with 3 framebuffers (mind the documented P4 PPA-freeze patch). Net: for SeedSigner's P4 landscape UI, prefer esp_lvgl_adapter TRIPLE_PARTIAL over esp_lvgl_port sw_rotate if both smoothness and tear-free rendering matter; otherwise esp_lvgl_port sw_rotate (avoid_tearing off) is the minimal route.

## All source URLs used
- https://github.com/espressif/esp-idf/issues/14639
- https://github.com/espressif/esp-bsp/issues/400
- https://github.com/espressif/esp-bsp/issues/665
- https://components.espressif.com/components/espressif/esp_lvgl_port
- https://components.espressif.com/components/espressif/esp_lvgl_port/versions/2.8.0~1/changelog
- https://components.espressif.com/components/espressif/esp_lvgl_port/versions/2.6.2
- https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/tools/esp_lvgl_adapter.html
- https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/lcd/mipi_dsi_lcd.html
- https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/lcd.html
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/ppa.html
- https://lvgl.io/docs/open/main-modules/display/rotation
- https://www.haraldkreuzer.net/en/news/waveshare-esp32-p4-module-dev-kit-c-compact-development-board-101-inch-dsi-display
- https://github.com/espressif/esp-iot-solution/issues/380
- https://github.com/lvgl/lvgl/issues/7698
