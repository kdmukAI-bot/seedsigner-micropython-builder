# Research: ESP32-P4 live camera preview fps on MIPI-DSI

This is a web-research deliverable (read-only). The "plan" is the cited synthesis itself,
returned to the caller as the final message. Findings captured below for the record.

## Bottom line
- Sensor (OV5647/SC2336/SC035HGS) can output 30-100 fps; the END-TO-END preview is gated downstream.
- Realistic full-screen DSI live preview at small/medium res: ~8-15 fps typical in naive/community
  builds; ~1-2 fps at large full-screen (M5Stack Tab5 1280x720). 30 fps is reachable only with a
  tight zero-copy / hardware-PPA / DMA path and modest buffer sizes.
- Dominant bottleneck = PSRAM bandwidth + full-frame copy / format-conversion (CPU memcpy or PPA
  contending for PSRAM), NOT the DSI peripheral and NOT the sensor. Espressif PPA docs say so verbatim.
- OV5647 Espressif driver has NO 480x480/VGA mode; smallest is 800x640 @ 50fps (RAW8); 1080p @ 30fps.

## Key sources (see final message for full cited list)
- esphome #16873 (DSI flush timing: 800x800 RGB888 ~42ms/frame, 51us/flush -> DSI not the limit)
- PPA doc (PSRAM bandwidth dependency verbatim)
- M5Stack Tab5 CNX review (1-2 fps full-screen)
- esp32.com t=46268 P4-EYE (480p ~15fps, SD-card limited for *recording*)
- Tasmota #24497 (640x480 MJPEG 38fps; nothing stable >960x720)
- OV5647 esp_cam_sensor driver source (mode table)
- ESP-Techpedia camera solution (sensor output fps != preview fps; "room for optimization")
