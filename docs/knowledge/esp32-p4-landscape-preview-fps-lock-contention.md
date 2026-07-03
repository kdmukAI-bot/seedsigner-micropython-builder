# ESP32-P4 landscape camera-preview fps is gated by LVGL-lock hold time, not the rotation cost

**Context:** Waveshare ESP32-P4 LCD 4.3 (ST7701 MIPI-DSI), landscape, image-widget camera
preview path (`board_pipeline_display_lvgl` push_frame_image_widget + the
`st7701_landscape_flush_cb` CPU rotation in `board_init.c`). MicroPython firmware preview ran
~8fps; the bare `scan_coord_test` app on the **same board_common** ran ~12fps. This explains why,
and why the obvious suspects are all wrong.

## The mechanism (this is the durable, non-obvious part)

The camera capture task pushes each frame to the display by **try-locking** the LVGL mutex:
`lvgl_port_lock(1)` (1ms) → `memcpy` the downscaled frame into the image widget's `cam_buf` →
`lv_obj_invalidate()` → unlock. If the lvgl task is holding the lock at that instant, the try-lock
**fails and the frame is silently skipped** (`lock_wait=0` in the metrics — it never waits).

The lvgl task holds the lock for the whole `lv_timer_handler`: **render (rasterize the camera image
+ composite the overlay widgets) + the 30ms landscape rotate-flush.** So:

```
lvgl task holds lock (render + 30ms rotation)  ──blocks──▶  camera push try-lock fails ──▶ frame skipped
```

The dependency is **one-directional**: LVGL never waits for the camera. When a push is skipped
nothing is invalidated, so the next `lv_timer_handler` does no render/blend/rotation and releases
quickly — the panel just holds the previous frame. Therefore **display fps = push-success rate**,
which is gated purely by *what fraction of the time the lvgl task is sitting on the lock*. Frame
availability is never the limit (the camera produces ~18fps PPA-bound, far above the ~8fps the
display consumes).

**Consequence:** anything that lengthens `lv_timer_handler` (a translucent overlay re-blended over
the camera every frame, a busier active screen, extra LVGL timers) widens the lock-hold window →
more camera pushes get skipped → lower preview fps — **even though the rotation cost itself is
unchanged.**

## Measurements that pin it (same board_common `c299ed3`, landscape, `CONFIG_CAM_PIPELINE_DEBUG`)

| build | disp | skip | DISP CPU (rotation) | DISP INTERVAL (feed) |
|---|---|---|---|---|
| firmware (MicroPython + overlay + UI) | ~8fps | 25% | 30ms | 125ms |
| scan_coord_test (bare app, no overlay) | ~12fps | 9% | 27ms | 80ms |

The rotation is identical (~30ms). The whole delta is the **skip rate** (9%→25%) — i.e. the
firmware's heavier per-frame LVGL render holds the lock longer, so the camera's push loses the race
more often.

## Ruled out (each with on-device evidence)
- **Priority** of the camera/lvgl tasks — A/B build at prio-5 measured the same fps as prio-1.
- **board_common code regression** — the bare app on the *current* board_common does 12fps.
- **The landscape rotation being slow** — same 27-30ms in both; it's the *lock-hold during it*,
  combined with the heavier render, that matters.
- **sdkconfig** (PSRAM 200MHz HEX, CPU 360MHz, L2 256KB, PM disabled, XIP) — byte-identical between
  firmware and the bare app.
- **Native USB / TinyUSB interrupt load** — firmware still ~8fps with the native cable physically
  unplugged and everything driven over UART.

## The fix — IMPLEMENTED + DEVICE-VALIDATED (2026-06-30)
**Move the rotate-flush off the LVGL lock** (deferred flush task): `st7701_landscape_flush_cb` hands
the rendered buffer to a dedicated `st7701_flush` task and returns without `lv_display_flush_ready`;
that task rotates + waits vsync + blits. A `flush_wait_cb` (registered with
`lv_display_set_flush_wait_cb`) makes LVGL's `wait_for_flushing` yield on a semaphore instead of
busy-spinning. This moves BOTH the ~30ms rotation AND the ~16ms vsync wait off the lock, so per-cycle
lock-hold drops from `render + rot + vsync` (~95ms) to `render` (~49ms).

**The non-obvious catch that makes or breaks it: you MUST switch the display from
`LV_DISPLAY_RENDER_MODE_DIRECT` to `FULL`.** In DIRECT double-buffered mode LVGL calls
`wait_for_flushing` from `refr_sync_areas()` — which runs **before** the render inside
`lv_display_refr_timer` (and also does a per-frame inter-buffer sync copy), all under the lock. With a
deferred flush pending, that pre-render wait re-holds the lock ~the same duration → benefit ≈ zero. In
FULL mode `refr_sync_areas` returns early (it's DIRECT-only) and the only cross-frame
`wait_for_flushing` is in `draw_buf_flush` **after** the render, so frame N's render overlaps the flush
task rotating frame N-1, and the lock-hold really collapses to `render`. (Verified against lv_refr.c
9.5: `refr_sync_areas` ~line 657 waits pre-render; `draw_buf_flush` ~line 1376 waits post-render.)
FULL and DIRECT use the same two full-size buffers → no extra memory.

**Threshold model for why the numbers move:** display fps is quantized by the ~56ms camera produce
cadence — a push is skipped whenever the lock-hold H overlaps the next camera frame. Synchronous H ≈
95ms > 56 → every-other frame skipped → ~8fps. Deferred H ≈ 49ms < 56 → pushes stop being skipped.

**Measured result (P4 LCD 4.3, via REPL `camera_scanner.start()`):** display **8→13.2fps**, push
**skip 25%→0%**, **lock_wait/hold = 0.0ms** (contention gone), DISP INTERVAL 125→75.5ms. Now beats the
old ~12fps bare-app reference. Rotation still ~31ms but off-lock. Remaining ceiling = CAM PPA downscale
(~65ms ≈ 15fps) — the next lever if more fps is wanted.

Safety notes that held: the `flush_wait_cb` gates buffer reuse until the task finishes, there are two
full-size buffers so render N+1 uses the other buffer while the task rotates N, and the rotation reads
the post-render buffer (not the camera `cam_buf`). Tuning knobs:
`BOARD_ST7701_FLUSH_TASK_{PRIORITY,STACK,AFFINITY}` in board_config.h — the flush task does NOT take
the LVGL mutex, so its priority is a pure CPU-scheduling knob (safe to raise if it gets starved).

**FULL-mode tradeoff to remember:** non-camera screens (menus, scroll) now re-render the whole 480×800
every refresh instead of only the changed region (DIRECT's partial render). Free for the camera screen
(the whole frame changes anyway); more per-refresh work for static UI — watch menu/scroll smoothness.

**Dummy-draw (stop LVGL, blit camera straight to panel) is the wrong fix here** — it kills the LVGL
widget overlay, which is a hard product requirement.

## Gotcha encountered alongside this
The `DISP CPU` / `CAM PPA` / `DISP INTERVAL` debug logs used `%lld`, but the firmware builds with
nano-printf (`CONFIG_LIBC_NEWLIB_NANO_FORMAT=y`), which has **no 64-bit conversion** — they printed
literal garbage (`avg=ld us  n=0`) and mis-parsed the rest of the varargs. Use `%d` with `(int)`
casts for microsecond values.
