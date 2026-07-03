# ESP32-P4 firmware: camera preview rotated + overlay off-screen = missing CONFIG_BOARD_LANDSCAPE

**Symptom (MicroPython firmware, P4 LCD 4.3, camera_scanner preview):**
- The live camera image appears rotated **90° CCW**, even though the SeedSigner UI (menus, back
  button, overlay text) is upright.
- The camera_scanner status-bar overlay renders **off the bottom of the screen** with a scrollbar,
  instead of on top of the camera square. Scrolling down reveals it.

**Root cause — one missing Kconfig line.** The board runs the display in **landscape at runtime**
(`display_manager.cpp` calls `board_init({.landscape = true})`), so the panel + all runtime-sized
LVGL screens are correct. But two pieces of the camera path key off the **compile-time** macro
`BOARD_LANDSCAPE`, which is set only by `CONFIG_BOARD_LANDSCAPE` (board_common `Kconfig`, default
`n`). The firmware's `sdkconfig.board` never set it, so `BOARD_LANDSCAPE == 0`:

1. **Camera rotation** — `board_pipeline.c`:
   ```c
   #if BOARD_LANDSCAPE && BOARD_DISPLAY_DRIVER == DISPLAY_ST7701
       int cam_rotation = (BOARD_CAMERA_ROTATION + 270) % 360;   // landscape compensation
   #else
       int cam_rotation = BOARD_CAMERA_ROTATION;                 // 0 -> image 90° off
   #endif
   ```
   Without the macro the camera loses its +270° PPA rotation → the preview is 90° CCW.

2. **Overlay geometry** — `board.h` defines the convenience macros
   `BOARD_DISP_H_RES/V_RES` as the physical dims **swapped only when `BOARD_LANDSCAPE`**. Unset →
   they stay portrait (480×800). `camera_scanner.cpp` sizes/places the overlay square from them:
   `sq_y = (BOARD_DISP_V_RES - square)/2` = `(800-480)/2 = 160` (should be 0 for a 480-tall
   landscape screen), so the status bar (bottom of the square) lands at y≈580 — off the 480-tall
   panel. Meanwhile the camera *image* container uses `lv_obj_center` against the real 800×480
   display, so image and overlay don't even align.

**Why it hid for so long:** the general SeedSigner UI queries the display size at **runtime**
(`lv_display_get_*_resolution()` → correct 800×480), so menus always looked fine. Only the two
camera-path sites above use the compile-time macros. And the camera overlay had literally never
rendered in the firmware before — `camera_scanner.start()` only began succeeding after the
LVGL-lock-starvation fix, and the preview only became watchable after the deferred-flush fps fix. So
these latent bugs surfaced together the first time the preview was actually looked at.

**Fix:** add `CONFIG_BOARD_LANDSCAPE=y` to the firmware's
`.../boards/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/sdkconfig.board`. Every bare board_common app
already sets this in its `sdkconfig.landscape`; the firmware overlay just needs to match. Verified on
device: PPA log flips to `rot=270`, overlay square lands at (160,0) with the status bar on the
image, and fps is unaffected.

**General lesson:** `CONFIG_BOARD_LANDSCAPE` is not purely cosmetic and is not implied by the runtime
`.landscape=true` flag. Runtime landscape drives the display flush + touch transform; the compile-time
Kconfig drives the `BOARD_DISP_*_RES` convenience macros and the camera-rotation compensation. Any app
that runs landscape AND uses `BOARD_DISP_*_RES` or the camera pipeline must set both.
