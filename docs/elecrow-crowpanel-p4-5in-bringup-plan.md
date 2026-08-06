# Elecrow CrowPanel Advance 5" ESP32-P4 — bring-up plan

**Status:** phases 0-3 complete — all four gates passed and device-verified, SD
included. Phase 4 is part-done: the SC2336 is detected and streams at 30 fps with
reopen-after-cancel working, but the gate is not met — no QR has been scanned yet and
colour/exposure are untuned. That last step needs a lit scene and a QR in front of the
lens, not more code.

**Board:** [CrowPanel Advanced 5inch ESP32-P4 HMI AI Display 800x480 IPS](https://www.elecrow.com/crowpanel-advanced-5inch-esp32-p4-hmi-ai-display-800x480-ips-touch-screen-with-wifi-6.html)
· vendor sources: [Elecrow-RD/-CrowPanel-Advanced-5inch-ESP32-P4-...](https://github.com/Elecrow-RD/-CrowPanel-Advanced-5inch-ESP32-P4-HMI-AI-Display-800x480-IPS-Touch-Screen)

Proposed names: `ELECROW_CROWPANEL_ADV_P4_50` (MicroPython board) /
`elecrow_crowpanel_adv_p4_50` (board_common board dir).

---

## Why this board is interesting

It sidesteps, by construction, the two problems that stalled the Waveshare
Touch LCD 5 (see `docs/knowledge/esp32-p4-lcd5-hx8394-bringup.md` and
`docs/csi-controller-leak-recovery-todo.md`):

- **The panel is natively landscape 800x480**, so there is *no* per-frame 90°
  rotation. On the Waveshare 5 that rotation costs ~83 ms of CPU per refresh and
  holds 3.7 MB of rotation buffers — and it scales *worse* than pixel count
  (`docs/knowledge/esp32-p4-dsi-rotation-cost-scaling.md`). Here it does not exist.
- **480 panel height reuses the existing `SUPPORT_DISPLAY_HEIGHT_480` asset
  tier** (same as the 4.3). No new baked font/logo tier, no
  `LV_FONT_FMT_TXT_LARGE` requirement, nothing to regenerate in the screens repo.

Both of those were the expensive parts of the Waveshare 5. What replaces them is
a different kind of work: an RGB display family and an I2C companion MCU.

## Verified hardware

All values below are from the vendor's own factory source
(`factory_sourcecode/V1.0/ESP32-P4-Advance-5inch-lvgl/`), not the wiki — Elecrow's
wiki pages are shared between their ESP32-S3 and ESP32-P4 variants of this panel
and mix specs (the S3 sibling uses an SC7277; this one does not use a panel
controller IC at all). Anything not confirmed on device is marked.

| item | value | source |
|---|---|---|
| SoC | ESP32-P4 rev v1.3 | `esptool flash_id` on the rig |
| Flash | **16 MB** (mfr 0xba, dev 0x4018) | `esptool flash_id` |
| PSRAM | 32 MB | product page |
| USB | enumerates as **`/dev/ttyUSB0`** (external bridge, not the P4's native USB) | rig |
| MAC | `e8:f6:0a:e3:f8:b6` | `esptool flash_id` |
| Display | **RGB parallel, 800x480, `esp_lcd_new_rgb_panel`** | `bsp_display.c` |
| Pixel clock | 25 MHz | `bsp_display.h` |
| Timing | HSYNC 4 / HBP 8 / HFP 8 · VSYNC 4 / VBP 16 / VFP 16 | `bsp_display.h` |
| RGB pins | HSYNC 40, VSYNC 41, DE 2, PCLK 3; DATA0-15 = 8,7,6,5,4,14,13,12,11,10,9,19,18,17,16,15 | `bsp_display.h` |
| Framebuffers | 2, `fb_in_psram`, 20-line bounce buffer | `bsp_display.c` |
| Touch | GT911, INT 42, RST 36, addr 0x5D/0x14 | `sdkconfig`, `bsp_display.h` |
| I2C | SDA 45, SCL 46 | `sdkconfig` (`CONFIG_I2C_GPIO_*`) |
| Companion MCU | **STC8H1K at I2C 0x2F** | `bsp_stc8h1kxx.h` |
| Camera | 2 MP MIPI-CSI header, reset via STC8 | product page + STC8 GPIO map |
| Audio | NS4168, amp shutdown via STC8 | product page + STC8 GPIO map |
| Radio | ESP32-C6-MINI-1 | product page |

### The STC8H1K companion MCU is the structural surprise

Several control lines are **not** MCU GPIOs — they hang off an STC8H1K 8051
reached over I2C at 0x2F. From its GPIO/PWM enums:

- `STC8_GPIO_OUT_LCD_BL_POWER` + `STC8_PWM_LCD_BL_EN` — backlight power and PWM
  (note `RGB_PIN_NUM_BK_LIGHT` is `-1`: there is no direct backlight GPIO)
- `STC8_GPIO_OUT_TP_RST` — touch panel reset
- `STC8_GPIO_OUT_CSI_RST` — camera reset
- `STC8_GPIO_OUT_AUDIO_SD` — audio amplifier shutdown
- plus battery telemetry (`stc8_battery_info_get`) and an LED state machine

This is conceptually the TCA9554 I/O-expander pattern `board_common` already
supports (`BOARD_HAS_IO_EXPANDER`), but wider: it carries PWM and battery state,
not just level-setting. **The backlight is on the critical path** — without the
STC8 driver the panel is dark even if RGB output is perfect, so this cannot be
deferred past phase 1.

Note the apparent redundancy: `CONFIG_TOUCH_GPIO_RST=36` *and*
`STC8_GPIO_OUT_TP_RST` both exist. Which one is wired needs an on-device check;
the GT911 driver already tolerates `GPIO_NUM_NC` (the Waveshare 5 uses that path).

## Open questions — answered by the phase 0 schematic trace

Answered from the Eagle netlist (`docs/board-schematics/elecrow-crowpanel-p4/`, which
is XML — parse it, don't read the PDF). Full pin/net detail now lives in hardware-kb
`elecrow/crowpanel-advance-5in-p4/board.md`; only the consequences are repeated here.

1. **Air gap — RESOLVED, and the pin is `GPIO20`, not GPIO54.** `GPIO20 —R95(0R)—`
   the C6's `EN`, 10K pull-up to `C6_VDD_3V3`, and GPIO20 has no other connection on
   the board. Driving it low holds the radio in reset, so `BOARD_RADIO_COPROC_RESET_PIN`
   works unchanged — with a **different pin value**.
   **`GPIO54` on this board is `SD2_CMD`** (the SDIO link *to* the C6); copying the
   other boards' `GPIO_NUM_54` would drive a data line. There is also a hardware air
   gap: `R76` (0R) is the sole power feed into `C6_VDD_3V3`.
2. **Camera populated? — YES**, confirmed on our unit.
3. **Which sensor? — SmartSens `SC2336`** (2 MP, 1280×720). SCCB scan finds 0x30 and
   chip-ID `0x3107/0x3108` reads `0xCB3A`; the vendor's own example agrees
   (`CONFIG_CAMERA_SC2336=y`). A **third** sensor for the fleet, but unlike the
   Guition's OV02C10 it needs no add-on component — `esp_cam_sensor` ships an SC2336
   driver. The SCCB bus is **its own** (GPIO33/34 — P4 side at 3.3 V, pulled up to
   `VDDPST_5`; BSS138 shifters Q6/Q7 gated on `DOVDD_1V8` put the *sensor* side at
   1.8 V, so firmware drives ordinary 3.3 V pins), reset is on **STC8 P1.3**
   (and the companion MCU releases it by default — the sensor answers without firmware
   touching it), and XVCLK comes from a **dedicated 24 MHz oscillator**, so there is no
   XVCLK GPIO to drive.
4. **SD pins — CONFIRMED** CLK 43 / CMD 44 / D0 39, and **1-bit only** (DAT1/2/3 are
   pull-up-only and never reach the P4).
5. **Touch reset — GPIO36 wins**, through three populated 0R links; the STC8's TP_RST
   branch is depopulated (`R122 = NC`). The vendor's `CONFIG_TOUCH_GPIO_RST=36` is right.
6. **Backlight is one line, not two.** Only **STC8 P1.1** → MT9201 `EN` matters.
   `STC8_GPIO_OUT_LCD_BL_POWER` (P3.7) is a no-op here — its FET is depopulated and a 0R
   link hardwires the rail on. A 10K pull-down means the backlight is **off at power-up**
   until the STC8 drives it.

## What is genuinely new work

`board_common` supports ST7796/ST7789 SPI, AXS15231B QSPI, and ST7701/HX8394
MIPI-DSI. It has **no RGB path**. That is the main lift:

- A new `DISPLAY_RGB` value in `board.h` and a `board_display_rgb.c`.
- **Do not reuse the DSI machinery.** `board_init.c` gates the deferred-flush
  task, the software landscape rotation, the portrait-scan mode switch and the
  camera compositing fences on `BOARD_DISPLAY_DRIVER == DISPLAY_ST7701`. None of
  it applies: this panel is landscape-native and needs no rotation. Resist the
  urge to generalise those gates into the RGB path — the win here is that the
  path is *simpler*.
- ⚠ **`lvgl_port_add_disp_rgb()` does NOT work on the ESP32-P4.** This corrects
  the original plan, which took the symbol's presence in the header for a
  supported path. Verified in our pinned `esp_lvgl_port` 2.7.2 *and* in the
  2.6.2 the board vendor ships: the body is wrapped in
  `#if CONFIG_IDF_TARGET_ESP32S3`, and every other target falls through to
  `ESP_RETURN_ON_FALSE(false, NULL, ... "RGB is supported only on ESP32S3")`. It
  returns NULL *after* internally creating the display, so a caller silently
  loses the handle rather than seeing a clean failure. Its tear-avoidance path
  is worse on the P4: it fetches framebuffers with
  `esp_lcd_dpi_panel_get_frame_buffer` — the MIPI-DSI accessor — because it
  assumes P4 implies DSI.
  The vendor's own working firmware runs with tearing-avoidance, direct mode and
  full refresh all **off**, and that is what `board_init.c` mirrors: create the
  LVGL display directly (as the ST7701 landscape branch already does), ordinary
  partial draw buffers, and a flush that copies each region into the framebuffer.
  `esp_lcd_panel_draw_bitmap()` is a synchronous CPU copy for an RGB panel, so
  the flush reports ready immediately — no completion callback, no semaphore.
- A new `board_stc8.c` (or an extension of the expander concept) for backlight
  power/PWM, touch/camera reset, and audio enable.
- `board_backlight.c` needs a non-LEDC branch that routes to the STC8.

## Phases

Each phase ends at a gate that can be checked on the rig. Do not proceed past a
red gate.

### Phase 0 — hardware truth (no firmware) — ✅ DONE, gate passed
The C6 hold-in-reset mechanism is identified (`GPIO20`), plus a hardware air gap
(`R76`). SD pins, touch reset, backlight and both I2C buses were resolved at the same
time. See the answered open questions above and hardware-kb for the pin map.

The factory 16 MB image **was** backed up before the first write, to
`.tmp/elecrow_factory_flash_16MB.bin` (gitignored, so re-read it if that tree is
cleaned and going back matters).

### Phase 1 — board scaffolding + STC8 + RGB display — ✅ DONE, gate passed
Shipped:
- Builder board def `ELECROW_CROWPANEL_ADV_P4_50` (`mpconfigboard.{h,cmake}`,
  `board.json`, `manifest.py`, `sdkconfig.board`) on the 16 MiB partition table.
- `build_firmware.sh`: `BOARD_CONFIG_DIR` map, `SEEDSIGNER_DISPLAY_HEIGHT=480`,
  `CHIP_TYPE=esp32p4`.
- `board_common`: new `DISPLAY_RGB` family (`board_display_rgb.c`), the
  `board_stc8.c` companion-MCU driver, a `BACKLIGHT_COMPANION` branch in
  `board_backlight.c`, and `boards/elecrow_crowpanel_adv_p4_50/`.

Two things the plan did not anticipate, both fixed:
- `lvgl_port_add_disp_rgb()` is S3-only — see the ⚠ note above and
  `docs/knowledge/esp-lvgl-port-rgb-is-s3-only.md`.
- **Backlight ordering.** Every other board configures the backlight as the very
  first act of `board_init()`. This one cannot: its backlight is an I2C register
  on the companion MCU, which does not exist until the bus is up. `board_init()`
  now defers the backlight init to a step 1b, after I2C + the companion MCU.
- **`display_manager` hardcoded the rotation swap.** It called
  `set_display(BOARD_LCD_V_RES, BOARD_LCD_H_RES)`, which is only right for a
  portrait panel rendered landscape; here it asked for a 480x800 profile and
  aborted. It now reads the resolution off the LVGL display handle, which is
  correct for every board and removes the duplicated assumption.

**Gate result (device-verified):** boots to the SeedSigner logo, centred, correct
colours, at 800x480; radio held in reset on GPIO20; companion MCU ACKs at 0x2F;
backlight dark at power-up (hardware pull-down) and lit once firmware writes the
PWM register. Colours being right also settles the byte-order question: **no byte
swap**, despite the vendor BSP setting one.

Not captured: the `DISP CPU`-equivalent timing the gate asked for. That
instrumentation is emitted by the camera pipeline's display path, which does not
run without a camera — so it lands in phase 4, not here.

**Portrait-board regression (both cross-board changes).** The `display_manager`
fix above and the touch-transform gating in phase 2 are not Elecrow-specific, so
both were checked against a rotated-portrait board. `GUITION_JC4880P443` compiles
clean, and the Waveshare P4-43 was flashed and exercised on device: ST7701 comes
up at 480x800, the GT911 still receives portrait maxes (so the landscape
transform is still applied there), `set_display()` resolves to the same 800x480
profile as before with no abort, the app boots, `DISP CPU` holds at ~19 ms, and
touch behaves as expected. No behaviour change on a rotated panel.

### Phase 2 — touch — ✅ DONE, gate passed
The GT911 came up as part of phase 1 (pins wired, driver initialises at
`x_max=800, y_max=480`, reports `TouchPad_ID:0x39,0x31,0x31`).

The one real decision was the axis transform. `board_init.c` used to apply
`swap_xy=1 / mirror_y=1` to the GT911 whenever `landscape` was set — the
transform for a *rotated portrait* panel. That is wrong here: the panel is
landscape-native and its controller already reports 800x480, so the swap would
map touches into a 480x800 space the display does not have. The transform is now
gated on `BOARD_DISPLAY_ROTATES_TO_LANDSCAPE` (see phase 1) rather than on the
`landscape` flag.

**Gate result (device-verified):** a 2x2 `main_menu_screen` with position-labelled
tiles, tapped in order top-left → top-right → bottom-left → bottom-right, returned
`button_selected` indices `0, 1, 2, 3` with matching labels, and each tile
highlighted under the finger. Both axes correct, no mirror, no swap.

Handy for future on-device checks: these screens are non-blocking (the call paints
and returns; taps land on a queue read via `poll_for_result`), so a screen can be
left up, tapped by a human, and its results read in a separate serial session —
provided the port does not reset the board on close. See the workflow reminders.
- The TP_INT pull-up is depopulated, so firmware owns the GT911 address strap
  during reset; let the driver probe both 0x5D and 0x14.
- **Gate:** navigate the app by touch.

### Phase 3 — storage + frozen app — ✅ DONE, gate passed
**Gate result (device-verified):** the self-booting dist runs the app from internal
flash with no microSD. `main.py` starts at 323 ms, the controller is ready at 735 ms,
and the Home menu (Scan / Seeds / Tools / Settings) renders at 800x480. The baked vfs
is 2,101,248 bytes at `0xc50000`; `0xc50000 + 0x3b0000 = 0x1000000`, so it fills the
16 MiB layout exactly, with no partition-table change needed.

**The SD path is device-verified too**, with `BOARD_HAS_SDCARD` on and
`BOARD_SD_WIDTH 1`: `sd_bus_width()` reports 1, `sd_ensure()` and `sd_live()` both
return True, the full 7.34 GiB volume is visible (`statvfs` 1,925,705 x 4096 blocks,
`namemax` 255 so LFN is live), and a 10,240-byte write/read round-trip comes back
byte-identical. Boot cost is ~365 ms (controller-ready 735 ms without a card,
1099 ms with one).

Note the diagnostic signature of an **empty slot**: every mount attempt returns
`sdmmc_init_ocr: send_op_cond (1) returned 0x107` (ESP_ERR_TIMEOUT), and the hotplug
poll repeats it once a second. That is indistinguishable from a wiring fault, so
confirm a card is actually seated before debugging the bus.

Card formatting matters and is not fully free-form: this firmware's FatFs is built
with `FF_FS_EXFAT 0` (`MICROPY_FATFS_EXFAT` is defined nowhere in the port), so an
**exFAT card will never mount** — which is the factory default for SDXC (>=64 GB).
Use FAT32. On-device `vfs.VfsFat.mkfs()` does exist, but it makes FAT16 as a
*superfloppy* (`FM_FAT | FM_SFD`, no partition table) and only falls back to
MBR-partitioned FAT32 when the card is too big for FAT16 — so for a card that also
has to be read by a desktop, format it off-device as MBR + FAT32.

Two things the plan did not anticipate:
- **This board has no on-chip LDO on the SD rail.** The rest of the P4 fleet powers the
  SDMMC rail from LDO channel 4 and must acquire it in firmware; here both the card VDD
  (`J5.VDD`) and the SoC IO domain carrying the pins (`VDDPST_5`, via `R25` 0R) sit on
  the board's `VDD_3V3`, and the LDO4 link `R109` is **NC**. Acquiring a channel would
  regulate an unloaded cap. That decision is now explicit per board in
  `BOARD_SD_PWR_LDO_CHAN` (0 here, 4 on the Waveshare P4s); a P4 board that declares
  `BOARD_HAS_SDCARD` without it fails the build rather than silently mis-powering.
- **The Python facade hardcoded a 4-bit bus.** `machine.SDCard(slot=0, width=4)` is a
  fleet assumption, not a board fact, and asking for 4 lines on a board that routes one
  cannot enumerate. The width now comes from the C side (`sd_bus_width()`).

Both are cross-board changes — see the regression note under phase 1.

### Phase 4 — camera — 🟡 streaming on device; gate not yet met
**Done and device-verified.** `BOARD_HAS_CAMERA` is on and the sensor is detected
(`sc2336: Detected Camera sensor PID=0xcb3a`). The pipeline streams 1280x720 at
**cam 30.0 fps / disp 15.0 fps / decode 14.5 fps** (gray 15.4 ms, quirc 50.9 ms) into
a 480x480 scan square, PPA `rot=0` (no rotation, as predicted for a landscape-native
panel), pillarboxed at `x_offset=160`. CPU: core1 100% (qr_decode 94%), core0 52%.

**The reopen case passes** — `stop()` then `start()` returns to streaming. That is the
exact failure that blocks the Waveshare 5, and it does not reproduce here.

**One fleet-wide fix was needed.** `board_pipeline.c` handed the CSI driver the main
I2C bus handle unconditionally, which is right only because every previous board hung
its sensor off the main bus. This is the first board with a **dedicated SCCB bus**, so
the sensor was probed on the wrong pins and detection NACKed — a failure identical to
an absent or unpowered sensor. The bus is now chosen from
`BOARD_CAM_SCCB_I2C_PORT == BOARD_I2C_PORT`; boards sharing the main bus are
unaffected. See `docs/knowledge/esp32-p4-camera-dedicated-sccb-bus.md`.

**Still owed by the gate:**
- **Scan a real QR.** Not yet attempted — needs a QR in front of the lens in usable
  light. Everything upstream of the decode is confirmed running.
- **Colour/exposure tuning.** The preview is a magenta, heavily-gained image in a dark
  room. `BOARD_CAMERA_TONE_GAMMA_X10` / `BOARD_CAMERA_TONE_BLACK_LEVEL` are unset, so
  the tone curve logs `disabled (linear)` and no black level is subtracted — the same
  shape as the Guition's pedestal trap, where the CCM tints the residual pedestal
  purple and AWB amplifies it (`docs/knowledge/guition-jc4880p443-camera-tuning.md`).
  Tuning needs a lit reference scene; do not guess values.
- **Orientation.** `BOARD_CAMERA_ROTATION` / `BOARD_CAMERA_MIRROR_Y` are left at 0
  pending a recognisable image to judge the mount against.

Note the IPA path here is cheaper than the Guition's: `esp_cam_sensor` ships
`cfg/sc2336_default.json` keyed `"SC2336"` — the sensor's own reported name — so
esp_video's lookup hits it and owns the ISP pipeline. `BOARD_CAMERA_IPA_CONFIG_NAME`
is therefore deliberately undefined. Defining it (to take ownership for per-session AE
metering, as the Guition does) means copying the tuning file under a key the sensor
does *not* report.

Original notes for the phase, retained:
- `CONFIG_CAMERA_SC2336=y` in `sdkconfig.board`. No new component needed —
  `esp_cam_sensor` ships this driver, unlike the Guition's OV02C10.
- Four differences from the rest of the fleet: SCCB is on its **own** bus (I2C2,
  GPIO33/34, so `BOARD_CAM_SCCB_I2C_PORT` is 1, not the main bus — the P4 pins are
  3.3 V; only the far side of the Q6/Q7 shifters is 1.8 V);
  **reset is an STC8 register** (`BOARD_STC8_OUT_CSI_RST`) rather than a GPIO, and
  is released by default; **XVCLK comes from a dedicated 24 MHz oscillator**, so
  neither XCLK Kconfig option applies; and the sensor is **1280×720**, not the
  1288×728 / 2592×1944 the other two produce.
- The SC2336 is RAW-Bayer with manual exposure/gain, like the OV02C10 — so it will
  need the same `esp_ipa` closed-loop AE/AWB treatment
  (`BOARD_CAMERA_IPA_CONFIG_NAME`) rather than relying on the sensor.
- The scan-path square is `min(800,480) = 480` with no cap needed. 480 is an exact
  2:3 of the sensor's 720 short axis, so the decoder sees the full sensor square.
  See `docs/knowledge/esp32-p4-camera-scan-path-geometry.md`.
- Watch PSRAM: the Waveshare 5's camera failure was contiguous-block exhaustion.
  This board's display costs ~768 KB (one framebuffer) against that board's ~13 MB
  of DPI + rotation + draw buffers, so it should have far more headroom — worth
  measuring rather than assuming.
- **Gate:** scan a QR, and re-open the camera after a cancel (the exact case the
  Waveshare 5 fails).

### Phase 5 — air gap + hardening
- `BOARD_RADIO_COPROC_RESET_PIN GPIO_NUM_20` (**not** 54 — see phase 0); confirm the
  network strip (`MP_DISABLE_NETWORK`) builds clean on this board.
- Note the residual window: GPIO20 is high-Z while the P4 is in reset, so the 10K
  pull-up lets the C6 boot its own flash until firmware pulls the pin low. Software
  can shorten that but not remove it — only desoldering `R76` (the sole feed into
  `C6_VDD_3V3`) is absolute. Decide whether the release target documents R76 removal.
- **Gate:** no radio component in the image, C6 provably held down.

## Workflow reminders

- One feature branch per repo, PR to `kdmukai/…` (never bot→bot). `board_common`
  is a submodule: commit and merge it **first**, then re-pin in the builder.
- Run `authorize-git N` from the **builder root**; `cd` into the submodule to run
  git there — never `git -C … commit`.
- **Two boards are on the rig now** (Waveshare 5 on ttyACM0/32 MB, Elecrow on
  ttyUSB0/16 MB). Always `esptool flash_id` and match flash size + MAC before
  `write_flash`.
- **Talking to this board's REPL takes care.** `RTS` drives `EN`, and pyserial
  asserts RTS on open — so a naive `serial.Serial(port)` holds the SoC in reset and
  the port goes silent forever. Pulse RTS deliberately (assert, release, wait out
  the boot) before typing. And because the companion MCU's reset is tied to the
  SoC's `CHIP_PU`, any reset also resets the STC8, which drops the backlight PWM to
  0 and blanks the panel. To leave a screen up across sessions: `stty -F <port>
  -hupcl`, and deassert RTS/DTR before closing.
- Keep the new board's changes off the `feat/waveshare-p4-lcd5-bringup` branches.
