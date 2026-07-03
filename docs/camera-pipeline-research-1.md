# Research: Is PSRAM bandwidth the dominant bottleneck for concurrent CSI+ISP+PPA+DSI on ESP32-P4?

Research-only task (web synthesis). Findings below; full prose returned to caller.

## Bottom line
Yes — official Espressif docs confirm PSRAM bandwidth is the shared chokepoint when camera, PPA, and display all hit PSRAM concurrently. Raw 16-line/HEX DDR bus peak ~800 MB/s; single-stream cached memcpy only ~185 MB/s due to cache-line-fill stalls; aggregate is shared across all DMA masters. Mitigation = keep working buffers in the 768 KB internal L2 SRAM / use bounce buffers.

## Key sources
- PPA docs (official, v6.0.2): "PPA performance highly relies on the PSRAM bandwidth ... When there are quite a few peripherals reading and writing to the PSRAM at the same time, the performance of PPA operation will be greatly reduced." https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/ppa.html
- LCD FAQ (official): "the upper limit of PCLK settings is constrained by the bandwidth of the PSRAM. Therefore, you need to enhance the PSRAM bandwidth" + bounce-buffer guidance. https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/lcd.html
- External RAM guide (official): HEX 16-line PSRAM @ 200 MHz. https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-guides/external-ram.html
- PIE discussion #353: measured PSRAM<->SRAM ~185 MB/s; SRAM<->SRAM ~775 MB/s. https://github.com/espressif/developer-portal/discussions/353
- Zephyr P4 features: 768 KB HP SRAM, 32 KB LP SRAM, up to 64 MB HEX/octal PSRAM. https://docs.zephyrproject.org/latest/boards/espressif/common/soc-esp32p4-features.html
- esp-idf #17967: camera+LCD contention, PCLK dropped 26->10 MHz, ~6 fps.
- arduino-esp32 #11651: PSRAM mis-clocked at 80 MHz vs 200 MHz tanks bandwidth.
