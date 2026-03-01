# README-INTEGRATION

## Branch
- `seedsigner-esp32s3`
- Purpose: carries local integration for SeedSigner-targeted ESP32-S3 board support in MicroPython, including board definition, partitioning, and external component wiring for ESP-IDF build flow.

## Key `ports/esp32` changes
- `ports/esp32/boards/WAVESHARE_ESP32_S3_TOUCH_LCD_35B/board.json`
- `ports/esp32/boards/WAVESHARE_ESP32_S3_TOUCH_LCD_35B/mpconfigboard.cmake`
- `ports/esp32/boards/WAVESHARE_ESP32_S3_TOUCH_LCD_35B/mpconfigboard.h`
- `ports/esp32/boards/WAVESHARE_ESP32_S3_TOUCH_LCD_35B/sdkconfig.board`
- `ports/esp32/partitions-16MiB-waveshare.csv`
- `ports/esp32/CMakeLists.txt`
- `ports/esp32/main/CMakeLists.txt`
- `ports/esp32/main/idf_component.yml`
- `ports/esp32/lockfiles/dependencies.lock.esp32s3`

## External dependency
- This integration depends on external components from:
  - https://github.com/kdmukAI-bot/custom-c-modules
