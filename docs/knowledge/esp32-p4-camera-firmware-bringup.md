# ESP32-P4 camera bring-up under MicroPython firmware (camera_scanner Step 3)

Bringing the OV5647 MIPI-CSI camera up from the **MicroPython firmware** (via the
`camera_scanner` module / `cam_pipeline_create`) surfaced **four** distinct issues
that the standalone C test app (`board_common/apps/scan_coord_test`) never hit,
because that app sets the right sdkconfig in its own `sdkconfig.defaults` and runs
its bring-up on `app_main`, not on the MicroPython VM task.

Board: Waveshare ESP32-P4 WiFi6 Touch LCD 4.3. OV5647 CSI shares **I2C port 0**
with the GT911 touch. Firmware board config:
`deps/micropython/mods/new_files/ports/esp32/boards/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/sdkconfig.board`
(a `new_files` overlay — edits apply by rsync at build, no patch regen needed).

The first three are **fixed** (camera now detects + streams). The fourth is the
**open blocker**: `start()` deadlocks on the LVGL port lock from the VM task.

---

## Symptom progression (each fix uncovered the next)

1. `cam_pipeline_create()` → `OSError('pipeline create failed')`, log
   `E pipeline_cam_csi: Failed to open /dev/video0` (after `esp_video_init`
   returned **OK** with no error). → **Fix #1**.
2. After #1: `open(/dev/video0)` succeeds, sensor detected, but `start()` **hangs**
   (the priming `DQBUF` never returns). → **Fix #2**.
3. After #2: pipeline creates + streams, then the board dies:
   `Guru Meditation Error: Core 1 panic'ed (Interrupt wdt timeout on CPU1)`. → **Fix #3**.
4. After #3: no crash, but `start()` **hangs hard** on the MicroPython VM task right
   after `cam_pipeline_create` returns (REPL fully unresponsive, no crash/flood). →
   **OPEN** (see below).

---

## Fix #1 — the sensor driver was never compiled in

`# CONFIG_CAMERA_OV5647 is not set` in the firmware build. The esp_video CSI/ISP
*video devices* are pulled in by board_common, but the **sensor driver**
(`esp_cam_sensor` OV5647) is per-application Kconfig — board_common's shared board
fragment only carries pins (`board_config.h`), so the sensor Kconfig lived in each
app's *own* `sdkconfig.defaults` (e.g. `scan_coord_test`) and the firmware never
inherited it.

Without the sensor driver, `esp_video_init()` initializes the CSI/ISP controller and
returns **`ESP_OK`** but probes/registers **no sensor**, so `/dev/video0` is never
created and `open()` fails. The "OK return + open fails" combo is the tell: it is NOT
an I2C/SCCB probe failure (that would make `esp_video_init` itself return an error).

```
CONFIG_CAMERA_OV5647=y
CONFIG_CAMERA_OV5647_MIPI_RAW10_1280x960_BINNING_45FPS=y
CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER=y
```

## Fix #2 — camera runtime config (sensor detected, but stream-start hung)

Three more per-app options from `scan_coord_test` that the firmware lacked:

```
CONFIG_ESP_VIDEO_DISABLE_MIPI_CSI_DRIVER_BACKUP_BUFFER=n  # firmware had it =y
CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=4096
CONFIG_BOARD_CSI_AE_TARGET=0
```

- **Backup buffer**: the firmware *disabled* the CSI driver's internal DMA backup
  buffer (`=y`). Without it the first frame stalls and the priming `DQBUF` inside
  `cam_pipeline_create` blocks forever — the observed `start()` hang.
- **Timer task stack 4096**: the ISP pipeline controller runs on the FreeRTOS timer
  task; the 2048 default overflows.

Also: the camera's `DQBUF` blocks the idle task. `scan_coord_test` disables the task
WDT entirely; we instead **kept** WDT + panic (for genuine hang detection) and only
dropped the idle-task checks:

```
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n
```

## Fix #3 — VERBOSE logging + esp_video's ISR = Interrupt-WDT crash

The firmware set `CONFIG_LOG_MAXIMUM_LEVEL_VERBOSE=y` (to allow runtime tag-bumping).
esp_video's CSI **DMA-done ISR** (`csi_video_on_trans_finished`, per DMA transfer)
calls `ESP_LOGD`. `ESP_LOGx` → `esp_log_is_tag_loggable()` → `esp_log_impl_lock_timeout()`
→ `xQueueSemaphoreTake()` — i.e. it **takes the log mutex**, which is illegal in an
ISR. With VERBOSE compiled in, that `ESP_LOGD` is emitted, the ISR blocks on the mutex
with interrupts disabled, and CPU1's **interrupt watchdog** fires the instant the
camera streams. Symbolized backtrace:

```
vListInsert → vTaskPlaceOnEventList → xQueueSemaphoreTake → esp_log_impl_lock_timeout
→ esp_log_is_tag_loggable → esp_log → csi_video_on_trans_finished
→ csi_dma_trans_done_callback → dw_gdma_channel_default_isr → shared_intr_isr
```

`scan_coord_test` never enabled VERBOSE, so that `ESP_LOGD` is a **compile-time no-op**
there — same camera, no crash. Cap the compiled-in maximum at INFO:

```
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_MAXIMUM_LEVEL_INFO=y   # was _VERBOSE; never use VERBOSE with the camera
```

This also kills the ISP IPA per-frame auto-exposure/histogram DEBUG flood (thousands
of lines/sec over the USB-JTAG console). Trade-off: no runtime tag-bump to VERBOSE;
re-enable ONLY for non-camera debugging.

**General rule:** with the camera enabled, do not compile in DEBUG/VERBOSE logs —
esp_video logs from ISR context and `ESP_LOGx` is not ISR-safe (it takes a mutex even
just to evaluate whether the line is loggable).

---

## RESOLVED — Fix #4: `start()` was LVGL-lock STARVATION (not a deadlock), fixed by flattening task priorities

**Resolution (2026-06-30):** Not a deadlock — **priority starvation** of the LVGL
recursive mutex. Measured: with the camera streaming, the esp_lvgl_port task (prio 5)
holds the lock through each `lv_timer_handler` (render + 30ms landscape rotate-flush),
and the CSI capture task (prio 5) takes it per frame. The MicroPython VM/consumer task
is prio 1 (`MP_TASK_COREID=1`, port-fixed). A FreeRTOS mutex serves the *highest-priority*
waiter, so the prio-1 task lost every race — `lvgl_port_lock(0)` for overlay-create took
**~54s** (instrumented), not forever. **Fix = flatten the LVGL-lock contenders to prio 1**
(== the VM): `BOARD_LVGL_TASK_PRIORITY` 5→1, `CSI_TASK_PRIORITY` 5→1, and
`CONFIG_CAM_PIPELINE_QR_TASK_PRIORITY=1`. At equal priority the mutex is FIFO/longest-
waiting-first, bounding the VM's wait to ~one render cycle. `start()` now returns ~0.3s;
A/B-confirmed the flatten does NOT cost preview fps. The earlier "app_main vs VM task"
hypothesis below was **wrong** — the discriminator was priority (1 vs 5), not the task
context; `scan_coord_test` (app_main, prio-1 consumer too) only "worked" because it
creates no overlay, so it never hit the contended lock.

— historical first diagnosis (kept for context; superseded by the above) —

After the three fixes the camera **detects, streams, and `cam_pipeline_create`
returns** (`I cam_pipeline: Pipeline created: 480x480`). But `cam_scanner_start()`
then **hangs** and never returns: the REPL goes fully unresponsive — no crash, no
reboot, no serial output, Ctrl-C ignored (the VM task is mutex-blocked, so the
KeyboardInterrupt is never serviced).

Isolated precisely: the last log is `cam_pipeline_create`'s own final line, so create
*returns*; the very next step in `cam_scanner_start` is `lvgl_port_lock(0)` (wait-
forever) to build the `camera_preview_overlay`. No "scanner started" log appears →
**the hang is that `lvgl_port_lock(0)`** while the camera is now streaming to the same
screen.

Why it works in `scan_coord_test` but not here: the C app runs the identical
pipeline→overlay→coordinator sequence on **`app_main`**, whereas the firmware runs it
on the **MicroPython VM task**. The signature is a priority/lock-contention or
priority-inversion between the just-started camera **capture task** (core 0) / the
`esp_lvgl_port` task and the VM task — the VM task can't acquire the port lock the
streaming display path holds/starves.

Candidate directions (need a deliberate decision + on-device verification; not yet
attempted):
- **Z-order constraint complicates reordering.** The overlay must be created *after*
  the pipeline's LVGL image so it draws on top, so simply building the overlay before
  starting the camera changes z-order. A `lv_obj_move_foreground` after the fact still
  needs the contended lock.
- Investigate the camera **display sink** (`board_pipeline_display_lvgl`) locking — does
  it hold `lvgl_port_lock` per frame, and at what task priority/core is the capture task
  pinned vs the VM task and `esp_lvgl_port`?
- Consider building the overlay/coordinator with the camera **paused**
  (`cam_pipeline_pause_frame_access`) and resuming after, or pinning the capture task
  off the VM task's core.

What IS proven at this point: the `camera_scanner` MicroPython binding layer
(import / `FRAME_*` consts / `read_status()` attrtuple / `report()` / error
propagation) and the camera hardware bring-up under firmware both work; the remaining
blocker is purely the VM-task ↔ LVGL-lock interaction during overlay creation.
