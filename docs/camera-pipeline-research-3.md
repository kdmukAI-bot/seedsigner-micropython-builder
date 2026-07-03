# esp-video-components Examples Inventory + Camera-to-Display Perf on ESP32-P4

## Headline finding
`video_lcd_display` is NOT in esp-video-components. It lives in **esp-iot-solution**
(`examples/camera/video_lcd_display`). Source-confirmed: it blits raw camera frames
straight to the MIPI-DSI LCD via V4L2 + `esp_lcd_panel_draw_bitmap`. **No LVGL, no UI
overlay, no PPA.** ISP does RAW8->RGB565.

No esp-video example overlays UI/LVGL on a live camera feed. None.

## esp-video-components examples (repo: espressif/esp-video-components, path esp_video/examples/)
- capture_stream — CSI 1280x720 RAW8/RGB565/RGB888/YUV420/YUV422 @ 30fps; DVP 640x480 RGB565 @ 6fps; SPI 240x320 YUV422 @ 10fps. Raw capture, no display/overlay. ISP for RAW8. P4/S3/C3/C5/C6/C61.
- image_storage/sd_card — CSI RAW8 1280x720 @30fps, DVP RGB565 640x480 @6fps. JPEG+H264+ISP. Stores to SD. No overlay. P4/S3.
- image_storage/usb_msc — same fmts/fps as sd_card. JPEG/H264/ISP. USB MSC storage. No overlay. P4/S3.
- m2m — memory-to-memory codec; 1280x720 JPEG/YUV420; encode ~30fps, decode ~19.75fps (per README sample output). JPEG/H264 enc + JPEG dec. No display. P4 (+S31).
- simple_video_server — 640x480 RGBP/JPEG over HTTP; OV2640 JPEG 640x480 25fps. JPEG enc. Raw streams in browser, no overlay. P4/S3/C3/C5/C6.
- uvc — CSI RAW8 1280x720 @30fps (H264), DVP RGB565 640x480 @6fps (JPEG). USB UVC out. H264+JPEG. P4 only. No overlay.
- v4l2_cmd — 1280x720, many fmts (BA81/RGBP/RGB3/YU12/422P/JPEG/H264). fps NOT documented. ISP+JPEG+H264. CLI only. P4/S3/C3/C5/C6.
- video_custom_format — 120x160 YUV422, 240x320 RGB565, 800x800 RAW8 @ 10/15/30fps. Optional ISP. No overlay. P4/S3/C3/C5/C6/C61.

## video_lcd_display (repo: espressif/esp-iot-solution)
- LCD 1024x600 EK79007 MIPI-DSI @60Hz; camera SC2336 RAW8 1280x720 @30fps MIPI 2-lane.
- ISP RAW8->RGB565/RGB888. NO PPA, NO LVGL, NO overlay. esp_lcd_panel_draw_bitmap.
- Display-output fps NOT documented (only camera 30fps stated). ESP-IDF v5.4+.

## ESP-IDF versions
Only video_lcd_display states a version (v5.4+). All esp-video-components READMEs: NOT documented.
