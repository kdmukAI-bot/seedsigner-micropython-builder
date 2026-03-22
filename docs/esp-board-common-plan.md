# Shared ESP Board Support — `esp-board-common`

## Context

The 2048-ESP32 and seedsigner-micropython-builder projects share ~90% identical ESP32 hardware init code (I2C, display, touch, PMIC, backlight, IO expander, LVGL port setup, AXS15231B RASET flush workaround). The 2048 project already has a proven config-driven multi-board architecture (`board_common`) supporting 4 Waveshare boards. This plan extracts that into a shared repo and integrates it into micropython-builder first. The 2048 project will be updated to consume it later.

A race condition bug in the LVGL port setup (discovered during the SeedSigner v9 migration but not yet fixed in 2048) demonstrates the real cost of duplicated code.

### Code Overlap

| 2048 `board_common/` | micropython-builder `ports/esp32/` | Notes |
|---|---|---|
| `board_i2c.c` | `esp_bsp/bsp_i2c.c` | Same I2C bus, same pins (GPIO 7/8), same mutex |
| `board_display_axs15231b.c` | `esp_bsp/bsp_display.c` | Same QSPI init, same 25-command init sequence |
| `board_touch_axs15231b.c` | `display_manager.cpp:touch_init()` | Same I2C touch config, same 400kHz fix |
| `board_pmic.cpp` | `esp_bsp/bsp_axp2101.cpp` | Same AXP2101 init |
| `board_backlight.c` | `esp_bsp/bsp_display.c` (brightness funcs) | Same LEDC PWM on GPIO 6 |
| `board_init.c:io_expander_init()` | `display_manager.cpp:io_expander_init()` | Same TCA9554 reset sequence |
| `board_init.c:lvgl_port_setup()` | `display_manager.cpp:lvgl_port_setup()` | Same LVGL port + RASET flush |

### Key Difference: Orientation

The same board (waveshare_s3_lcd35b) is used in **portrait** by 2048 and **landscape** by SeedSigner. This affects the flush callback (memcpy vs per-pixel rotation), LVGL dimensions (native vs swapped), and touch mapping. Orientation is an **application-level parameter**, not a board property.

### What changes for micropython-builder

- `ports/esp32/esp_bsp/` — **deleted** (absorbed into board_common drivers)
- `ports/esp32/display_manager/display_manager.cpp` — shrinks from ~250 to ~30 lines (calls `board_init()` + `run_screen()` wrapper)
- `ports/esp32/display_manager/CMakeLists.txt` — depends on `board_common` instead of `esp_bsp`
- Board-specific peripheral drivers (camera, RTC, IMU, audio, SD) move to board_common
- New submodule at `ports/esp32/board_common/`

## New Repo: `esp-board-common`

### Source material

Start from 2048-ESP32's `components/board_common/` (proven, hardware-tested) + `boards/*/board_config.h`. Extend with:
1. **Orientation support** — `board_app_config_t` with `landscape` flag
2. **Race condition fix** — `lvgl_port_lock()` across `add_disp` + callback overrides
3. **SeedSigner peripherals** — camera, RTC, IMU, audio, SD card drivers (from micropython-builder's `esp_bsp/`)
4. **Extended board configs** — add peripheral pin defines to existing board configs

### Directory structure

```
esp-board-common/
  board.h                                  # Interface + enums + board_app_config_t
  board_init.c                             # Generic dispatcher (NO app_main)
  board_i2c.c/.h                           # from 2048 board_i2c
  board_backlight.c/.h                     # from 2048 board_backlight
  board_pmic.cpp/.h                        # from 2048 board_pmic
  board_display_axs15231b.c/.h             # from 2048
  board_display_st7796.c/.h                # from 2048
  board_display_st7789.c/.h                # from 2048
  board_touch_axs15231b.c/.h               # from 2048
  board_touch_ft6336.c/.h                  # from 2048
  board_touch_cst816d.c/.h                 # from 2048
  board_camera.c/.h                        # from micropython-builder esp_bsp/bsp_camera
  board_sdcard.c/.h                        # from esp_bsp/bsp_sdcard
  board_audio.c/.h                         # from esp_bsp/bsp_es8311
  board_rtc.c/.h                           # from esp_bsp/bsp_pcf85063
  board_imu.c/.h                           # from esp_bsp/bsp_qmi8658
  boards/
    waveshare_s3_lcd35b/board_config.h     # Extended with camera/RTC/etc. pins
    waveshare_s3_lcd35/board_config.h
    waveshare_p4_lcd35/board_config.h
    waveshare_s3_lcd2/board_config.h
  CMakeLists.txt
  idf_component.yml                        # All display/touch/peripheral deps
```

### Key interface: `board.h`

```c
#pragma once
#include "lvgl.h"

/* Driver enums */
#define DISPLAY_ST7796      1
#define DISPLAY_ST7789      2
#define DISPLAY_AXS15231B   3
#define TOUCH_FT6336        1
#define TOUCH_CST816D       2
#define TOUCH_AXS15231B     3
#define PMIC_AXP2101        1

extern const int LCD_H_RES_VAL;
extern const int LCD_V_RES_VAL;

/* Application-level config (not hardware — passed by the consuming project) */
typedef struct {
    bool landscape;     /* true = 90° CW rotation in flush + touch transform */
} board_app_config_t;

int board_init(const board_app_config_t *app_cfg,
               lv_display_t **disp, lv_indev_t **touch_indev);
void board_run(void);
/* No app_main(), no game_main() — each project defines its own entry point */
```

### Orientation handling in board_init.c

When `app_cfg->landscape == true` on a RASET-bug board:
- LVGL dimensions swapped: `hres = LCD_V_RES, vres = LCD_H_RES`
- Flush callback: per-pixel 90° CW rotation + byte swap (from micropython-builder)
- Touch: `swap_xy=1, mirror_x=1` added to transform portrait touch → landscape

When `landscape == false` (or non-RASET boards):
- LVGL dimensions: native `hres = LCD_H_RES, vres = LCD_V_RES`
- Flush callback: `memcpy` + `lv_draw_sw_rgb565_swap` (from 2048)
- Touch: no transform

Two separate flush callbacks selected at init time (no runtime branching in the hot path).

### Race condition fix (both orientations)

```c
lvgl_port_lock(0);
*disp_out = lvgl_port_add_disp(&disp_cfg);
#if BOARD_DISPLAY_QUIRK_RASET_BUG
    lv_display_set_flush_cb(*disp_out, landscape ? landscape_flush_cb : portrait_flush_cb);
    esp_lcd_panel_io_register_event_callbacks(io_handle, &io_cbs, *disp_out);
#endif
lvgl_port_unlock();
```

### Board config extensions for SeedSigner peripherals

Example additions to `boards/waveshare_s3_lcd35b/board_config.h`:
```c
/* ── Camera (DVP) ── */
#define BOARD_HAS_CAMERA            1
#define BOARD_PIN_CAM_XCLK          GPIO_NUM_38
#define BOARD_PIN_CAM_PCLK          GPIO_NUM_41
#define BOARD_PIN_CAM_VSYNC         GPIO_NUM_17
#define BOARD_PIN_CAM_HREF          GPIO_NUM_18
#define BOARD_PIN_CAM_Y2            GPIO_NUM_45
/* ... Y3-Y9 ... */
#define BOARD_CAM_XCLK_FREQ        (20 * 1000 * 1000)

/* ── SD Card ── */
#define BOARD_HAS_SDCARD            1

/* ── Audio (ES8311) ── */
#define BOARD_HAS_AUDIO             1

/* ── RTC (PCF85063) ── */
#define BOARD_HAS_RTC               1

/* ── IMU (QMI8658) ── */
#define BOARD_HAS_IMU               1
```

2048 never references `BOARD_HAS_CAMERA` etc. — the code compiles out via `#if` guards.

### External dependencies

**Remain as separate components (not inside board_common):**
- `XPowersLib/` — third-party AXP2101 library, referenced by `board_pmic.cpp`
- `esp32-camera/` — full camera HAL with 15+ sensor drivers, referenced by `board_camera.c`

These stay in micropython-builder's `ports/esp32/` as sibling components.

**`esp-board-common/idf_component.yml`:**
```yaml
dependencies:
  espressif/esp_lcd_axs15231b: "2.1.0"
  espressif/esp_lcd_st7796: "1.4.0"
  espressif/esp_lcd_touch_ft5x06: "1.1.0"
  espressif/esp_lcd_touch_cst816s: "1.1.0"
  espressif/esp_io_expander_tca9554: "2.0.3"
```

## Changes to micropython-builder

### Submodule

```bash
git submodule add /home/kdmukai/dev/esp-board-common ports/esp32/board_common
```

Local path initially (GitHub account suspended); switch to remote URL when account is restored.

### display_manager.cpp (rewrite — ~30 lines)

```cpp
#include "board.h"
#include "display_manager.h"

static lv_display_t *lvgl_disp = NULL;
static lv_indev_t *lvgl_touch_indev = NULL;

extern "C" void init(void) {
    board_app_config_t cfg = { .landscape = true };
    board_init(&cfg, &lvgl_disp, &lvgl_touch_indev);
}

extern "C" const char *run_screen(display_manager_ui_callback_t cb, void *ctx) {
    if (!cb) return "null screen callback";
    if (!lvgl_port_lock(0)) return "display lock unavailable";
    const char *err = NULL;
    try {
        cb(ctx);
    } catch (const std::exception &e) {
        err = e.what();
    } catch (...) {
        err = "unknown exception in screen callback";
    }
    lvgl_port_unlock();
    return err;
}
```

### display_manager/CMakeLists.txt

```cmake
idf_component_register(
    SRCS "display_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES "board_common" "esp_lvgl_port" "lvgl" "seedsigner"
)
```

No longer depends on `esp_bsp`, `espressif__esp_io_expander_tca9554`, `espressif__esp_lcd_axs15231b` — those are board_common's dependencies.

### display_manager.h

Unchanged — `init()`, `run_screen()`, callback typedef stay the same.

### bindings/micropython.cmake

Update include paths:
```cmake
target_include_directories(usermod_dm INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/display_manager
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/board_common    # was esp_bsp, esp_lv_port
    ${SEEDSIGNER_C_MODULES_DIR}/components/seedsigner
)
```

### Delete

- `ports/esp32/esp_bsp/` — entire directory (all code absorbed into board_common)

### Keep

- `ports/esp32/esp32-camera/` — external camera HAL component
- `ports/esp32/XPowersLib/` — external PMIC library
- `ports/esp32/display_manager/` — thin MicroPython wrapper (rewritten)

### MicroPython patch

Update `idf_component.yml` hunk — remove display/touch/IO expander deps (now in board_common's manifest). Keep only:
```yaml
lvgl/lvgl: "9.5.0"
espressif/esp_lvgl_port: "2.7.2"
espressif/esp_lcd_touch: "1.2.1"
espressif/esp_codec_dev: "1.5.4"
```

### sdkconfig.board

May need minor updates if board_common's CMakeLists.txt introduces new IDF component requirements.

## Branching Strategy

The `feature/lvgl-v9-migration` branch (2 commits: `e51ebe8` + `33e0d21`) has not been pushed yet (GitHub account `kdmukAI-bot` suspended). This multi-board refactor builds on top of that work.

```
main
  └── feature/lvgl-v9-migration (existing, unpushed)
        └── feature/multi-board-refactor (NEW, branched from lvgl-v9-migration)
```

When GitHub account is reinstated:
1. Push `feature/lvgl-v9-migration`, open PR → merge to `main`
2. Push `feature/multi-board-refactor`, open PR targeting `main` (or rebase onto `main` after v9 merge)

Create the new branch:
```bash
git checkout feature/lvgl-v9-migration
git checkout -b feature/multi-board-refactor
```

## Implementation Steps

### Phase 1: Create `esp-board-common` repo

1. `git init` new repo at `/home/kdmukai/dev/esp-board-common/`
2. Copy 2048's `board_common/` files: `board.h`, `board_init.c`, all driver files, `CMakeLists.txt`, `idf_component.yml`
3. Copy 2048's `boards/` directory (4 board configs)
4. Modify `board.h`: add `board_app_config_t`, remove `game_main()` declaration
5. Modify `board_init.c`:
   - Remove `app_main()` definition
   - Add `board_app_config_t` parameter to `board_init()`
   - Add landscape flush callback (from micropython-builder's current `axs15231b_flush_cb`)
   - Add landscape LVGL dimension swapping
   - Add landscape touch transform (`swap_xy=1, mirror_x=1`)
   - Add race condition fix (`lvgl_port_lock()` across `add_disp` + callback overrides)
6. Add SeedSigner peripheral drivers: adapt from `esp_bsp/bsp_camera.c`, `bsp_es8311.c`, `bsp_pcf85063.c`, `bsp_qmi8658.c`, `bsp_sdcard.c` → parameterized `board_camera.c`, etc.
7. Extend board configs with peripheral pin defines
8. Update `CMakeLists.txt` and `idf_component.yml` for new drivers
9. Initial commit

### Phase 2: Integrate into micropython-builder

1. `git checkout -b feature/multi-board-refactor` from `feature/lvgl-v9-migration`
2. Add `esp-board-common` as submodule at `ports/esp32/board_common/` (local path initially)
3. Rewrite `display_manager.cpp` (~30 lines, calls `board_init()`)
4. Update `display_manager/CMakeLists.txt`
5. Update `bindings/micropython.cmake` include paths
6. Delete `ports/esp32/esp_bsp/` directory
7. Update MicroPython patch (remove display/touch deps from `idf_component.yml`)
8. Verify `MICROPY_EXTRA_COMPONENT_DIRS` picks up board_common + its deps

### Phase 3: Build and test

1. `make docker-build-all` — full Docker build
2. Verify managed_components downloads correct versions
3. Flash to hardware: display renders, touch responds, camera works
4. Verify no race condition (clean display on boot, no shredded frames)

### Phase 4 (future): Update 2048-esp32

Not part of this plan. After micropython-builder is working:
1. Replace 2048's `components/board_common/` with submodule
2. Move `app_main()` to `main/game_main.c`
3. Pass `{ .landscape = false }` to `board_init()`

## Notes

- **Repo hosting**: GitHub account (`kdmukAI-bot`) is suspended. `esp-board-common` will be created locally first. Submodule uses local path `/home/kdmukai/dev/esp-board-common`. Push and switch to remote URL when account is restored.
- **sdkconfig**: Split between board hardware (esp-board-common `boards/<board>/sdkconfig.defaults`) and application-layer (MicroPython `sdkconfig.board`). The board hardware sdkconfig is injected via `BOARD_CONFIG_DIR` in each board's `mpconfigboard.cmake`; `sdkconfig.board` contains only MicroPython-specific settings and can override anything.
- **2048 race condition**: `board_init.c:169-178` in 2048-esp32 is missing `lvgl_port_lock()` around the flush callback override. Will be fixed when 2048 adopts the shared board_common.

## Verification

- `make docker-build-all` produces working firmware
- display_manager.cpp is ~30 lines (just `board_init()` + `run_screen()`)
- No `esp_bsp/` directory remains
- Hardware test: display renders correctly in landscape, touch works, no race condition
- Screenshot gallery renders correctly

## DONE: sdkconfig deduplication (2026-03-22)

Hardware settings (flash, SPIRAM, SPI ISR, PM, CPU freq) now come from
esp-board-common's `boards/<board>/sdkconfig.defaults`, injected into each
board's `mpconfigboard.cmake` via the `BOARD_CONFIG_DIR` CMake variable.

Each `sdkconfig.board` was slimmed to only MicroPython-specific settings:
LVGL fonts, BT/WiFi, partition table, WDT timeout, SPIRAM strictness,
SPI slave ISR. Duplicated `CONFIG_ESPTOOLPY_*`, `CONFIG_SPIRAM_*`,
`CONFIG_PM_ENABLE`, and `CONFIG_SPI_MASTER_ISR_IN_IRAM` were removed.

## DONE: esp-board-common layout changes (2026-03-22)

The `feat/camera-viewfinder` branch was rebased onto `main` (which had the
P4 board support commit). The combined `main` now includes:
- Source files in `src/` (include dir is `src/`, not `.`)
- `board_config.h` include dirs PUBLIC
- `board_camera_init()` takes `framesize_t frame_size` parameter
- `board_set_render_interval_ms()` API in `board.h`
- `idf_component.yml` includes `esp_codec_dev`, `XPowersLib`, `esp_cam_sensor`
- `boards/<board>/sdkconfig.defaults` for all boards (S3 35B, S3 35, P4)
- `board_camera_csi.c` in `src/` for ESP32-P4 MIPI-CSI support

micropython-builder adaptations:
- Submodule updated to new `main`
- `bindings/micropython.cmake` include path: `board_common` → `board_common/src`
