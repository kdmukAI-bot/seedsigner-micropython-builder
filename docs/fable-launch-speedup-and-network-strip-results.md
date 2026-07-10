# Results: Network strip → dependency prune → launch speedup (ESP32-P4)

**Living morning report — updated after every checkpoint.**
Run started 2026-07-10 (overnight). Executor: Fable 5. Brief: `docs/fable-launch-speedup-and-network-strip-todo.md`.
Board: `WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43` only.

## Status snapshot

| Priority | Branch | State |
|---|---|---|
| P1 network strip | `feat/p4-network-strip` | IN PROGRESS — baseline captured |
| P2 dependency prune | (not started) | pending |
| P3 launch speedup | (not started) | pending — baseline timing being measured |

`main` is untouched at c005e0d (PR #27 merge). Nothing pushed/merged/PR'd.

## Branches & commits

- `feat/p4-network-strip`
  - `3d5d891` chore(deps): bump seedsigner-lvgl-screens to upstream main (267cc64) — picks up screens PRs #64/#65/#66. **Build-verified** (full docker-build-all incl. screenshot generator, 123 scenarios OK).

## Baseline (before any strip) — captured 2026-07-10

Build: `feat/p4-network-strip` @ 3d5d891, clean `make docker-build-all`, BOARD=WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43.
Artifacts preserved in session scratchpad (`baseline/`: micropython.map, project_description.json, sdkconfig).

### Network stack presence (the P1 target)

- `FINAL_IDF_COMPONENTS` (from build STATUS line) includes: `bt esp_eth esp_netif esp_wifi lwip` (+ the rest of the default list). `MICROPY_DISABLE_NETWORK=OFF`, `MAIN_EXCLUDE_COMPONENTS=` (empty) — dormant strip path confirmed never enabled.
- Linker map (`micropython.map`) network refs: **4538** lines matching wifi/lwip/netif/nimble/bt/wpa.
  - Archives contributing sections: `liblwip.a` (3277), `libesp_netif.a` (452), `libesp_wifi.a` (7), `libesp_eth.a` (7).
  - **No** `bt`/`nimble`/`wpa_supplicant` code in the map — `CONFIG_BT_ENABLED=n` already keeps BT compile-dead even though the `bt` component is required.
- Full archive inventory (network-related, all present in baseline link):
  `lwip, esp_netif, esp_wifi, esp_eth, esp-tls, esp_http_client, esp_http_server, esp_https_server, esp_https_ota, esp_local_ctrl, mqtt, tcp_transport, http_parser, protocomm, protobuf-c, esp_hid, espressif__mdns, espressif__esp_wifi_remote, espressif__esp_hosted, espressif__eppp_link, espressif__esp_serial_slave_link`
- Notable non-network P2 candidates also in the link: `espressif__esp_codec_dev` (audio), `espressif__esp_h264`, `fatfs`+`wear_levelling`+`spiffs` (IDF-side; MP uses its own oofatfs), `esp_lcd_axs15231b`+`esp_lcd_st7796` (other boards' LCDs), `esp_lcd_touch_cst816s`+`ft5x06` (other boards' touch), `esp_io_expander`+`tca9554` (BOARD_HAS_IO_EXPANDER=0 on P4-43), `waveshare__pcf85063a` (RTC), `waveshare__qmi8658` (IMU), `cmock`/`unity`/`app_trace`/`esp_gdbstub` (test/debug), `mqtt`, `json`.
  (Archive presence in the map ≠ bytes in the image for every case — some may contribute zero sections; the prune pass will use the component graph + size tools for byte-level truth.)

### Launch timing baseline — **~10.9 s power-on → logo-slide** (measured 2026-07-10)

Method: hard reset via USB-Serial-JTAG RTS toggle (pyserial, host-side — no docker/esptool latency),
30 fps webcam video, per-frame luma-transition analysis, cross-checked against device-clock serial lines.
The brief's "~7 s" was an estimate; the measured baseline is ~10.9 s.

| Event | Video time | Since reset |
|---|---|---|
| RTS reset (wallclock-corroborated) | 10.80 s | 0.0 s |
| Display-init white flash | 13.37–13.80 s | **+2.6 s** |
| Static C-boot logo appears, held | ~13.8 s | **+3.0 s** |
| **OpeningSplash logo-slide onset (THE metric)** | ~21.75 s | **+10.95 s** |
| Splash version/credits screens | 23.2–25.2 s | +12.4→14.4 s |
| Home rendered | ~25.5 s | +14.7 s |

Serial cross-check (device ms-clock): Python locale print ~10.2 s, `sdmmc_periph` re-init msg 10.25 s,
first LVGL flush stats (`DISP CPU … n=3`) 10.54 s → consistent with slide at ~10.9 s wall.

**Attribution (baseline):** ~3.0 s firmware (ROM+bootloader+**SPIRAM memtest**+IDF init+display/SD init)
→ then **~7.9 s MicroPython VM boot + frozen imports + `import seedsigner.controller` chain** up to
`Controller.start()`. The Python phase dominates (~72%). Network-stack boot init lives inside the 3.0 s
slice, so P1/P2's timing upside is bounded there; the 7.9 s Python phase is app-side (brief: measure +
report only). P3's `SPIRAM_MEMTEST=n` + log-level cuts also attack the 3.0 s slice.

Serial-capture gotchas (P4-43, this firmware): console primary is UART0; `/dev/ttyACM0` (USB-Serial-JTAG)
carries early-boot board logs + Python prints + app-phase ESP_LOG only. The port re-enumerates on reset —
`stty -F /dev/ttyACM0 115200 raw -echo` must be re-run *after* re-enumeration (~2.5 s post-reset) or reads
truncate. `CONFIG_BOARD_LOG_TO_FLASH` is **not** enabled in this firmware (no `log_store` partition) — the
esp-build skill's flash-log-dump flow does not apply; full early-boot (ms) milestones need UART0 or the
P3 instrumentation pass.

### Baseline network Kconfig surface (from resolved `sdkconfig`)

`CONFIG_ETH_ENABLED=y` (+EMAC/RMII/SPI-eth drivers), `CONFIG_ESP_NETIF_TCPIP_LWIP=y`, full
`CONFIG_ESP_WIFI_*` buffer/AMPDU/WPA3 set, **85** `CONFIG_LWIP_*` lines. All targets for `=n` overrides.

## P1 — network strip (in progress; statically clean since build #1)

**Image-level result (already achieved by build iteration #1):** zero bytes from any network
component in the linked image — placed-archive-member count for
lwip/esp_netif/esp_wifi/esp_eth/bt/esp-tls/http/mqtt/protocomm/etc: **0** (baseline: **3709**).
`esp_hosted`, `esp_wifi_remote`, `eppp_link`, `esp_serial_slave_link`, `mdns` are gone entirely
(removed from `main/idf_component.yml` — they were explicit P4 deps in upstream's manifest).

**Why the prior `MICROPY_DISABLE_NETWORK` machinery never worked — two root causes found:**

1. **ESP-IDF script-mode pass can't see `-D` cache args.** IDF's early component expansion
   evaluates each component's CMakeLists in *script mode* to collect the REQUIRES graph. In that
   pass `MICROPY_DISABLE_NETWORK` (a `-D` cache var) is undefined, so `esp32_common.cmake`
   reported the *networked* IDF_COMPONENTS list — the network components entered the graph and
   compiled even when the real pass linked the minimal list. Same quirk family as the screens
   repo's `screen_sources.cmake` script-mode fix. **Fix:** the flag detection now falls back to
   the *environment variable* `MICROPY_DISABLE_NETWORK` (visible in script mode);
   `build_firmware.sh` exports it for the real build.
2. **`EXCLUDE_COMPONENTS` in `main/CMakeLists.txt` is silently ignored.** IDF only honors it at
   *project level*, set before `project.cmake` is included. The prior attempt set it inside the
   `main` component — a no-op that *looked* load-bearing. **Fix:** exclusion moved to
   `ports/esp32/CMakeLists.txt` (which the patch already modifies), covering the whole
   radio/TCP-IP tree incl. defensive entries for the removed managed components.

**tinyusb kept lwip/esp_netif in the compile graph** (not the image): it unconditionally compiles
its USB networking classes (ECM/RNDIS/NCM) and requires `esp_netif` for one header, though zero
bytes of them are placed (CFG_TUD networking off). **Fix:** new managed-component patch
`component_patches/tinyusb-no-usb-net-class.patch` (mechanism precedent: the LVGL PSRAM patches)
drops the NET class sources + the esp_netif requirement at the source level — USB networking is
now not even compiled. Build order note: the `submodules` fetch pass runs unstripped, component
patches apply, then the real build runs with the strip active (fresh-tree safety).

**C6 co-processor rendered inert (hardware level):** the P4-43 follows the esp-hosted P4
reference wiring — C6 reset line on **GPIO54**, idles high (C6 runs its factory hosted-slave
firmware), **low = held in reset**; polarity verified from esp_hosted 2.7.0 Kconfig
(SDIO reset default ACTIVE_HIGH ⇒ "Low will trigger reset"), pin from its P4 board preset;
GPIO54 unused by our board config (audio PA is GPIO53). `board_common` change (branch
`feat/radio-coproc-hold-in-reset` in that submodule): generic `BOARD_RADIO_COPROC_RESET_PIN`
hook in `board_init.c` — configured output-low + pull-down at the earliest point of board init
and left low; P4-43 `board_config.h` defines it. Even before this, nothing ever started the
SDIO host (upstream's `sdkconfig.p4_wifi_*` fragments were never in our board's config chain,
and now esp_hosted isn't even fetched) — the GPIO hold makes the C6 execute no code at all.

**sdkconfig belt-and-suspenders** added to the P4-43 `sdkconfig.board` (=n for
WIFI/WIFI_REMOTE/HOSTED/NETIF-TCPIP/ETH; BT was already =n). Note: kconfig *choice* symbols
(e.g. `ESP_NETIF_TCPIP_LWIP`) can't be forced off while their component is discovered — the
real guarantee is component absence + zero placed members + boot-log proof.

**Build iterations:** #1 flag+manifest probe (clean, image clean) → #2 env fix + sdkconfig
(fail: `main.c` unconditional `#include "modnetwork.h"` → guarded with
`MICROPY_PY_NETWORK || MICROPY_PY_SOCKET_EVENTS`) → #3 clean → #4 (running) project-level
exclusion + tinyusb patch + C6 hold-in-reset.

### P1 runtime validation — PASSED on device (2026-07-10)

**Boot-log proof (timestamped serial capture, stripped firmware):**
- `I (1873) board: Radio co-processor held in reset (GPIO54 low)` — C6 hold active, board init unaffected.
- **Zero** wifi/phy/netif/lwip/BT/SDIO-slave/esp-hosted init lines anywhere in the boot.
- Healthy bring-up end-to-end: PSRAM 32MB@200MHz memtest OK (1.72 s), ST7701 display, GT911 touch, LVGL task, SD VDD, locale set, app renders.

**Launch timing (same video/luma method as baseline): 10.95 s → 10.08 s (−0.9 s from P1 alone).**
Serial cross-check: first LVGL flush stats at device-clock 9.73 s (baseline 10.54 s).

**Device regression sweep (all PASS):**
| Test | Method | Result |
|---|---|---|
| Boot → Home | webcam | PASS |
| Network absent (Python) | `import network/socket/espnow/bluetooth/ssl` | all ImportError (PASS) |
| SD card | `/sd` listing (22 lang-pack dirs) + file read | PASS |
| Native crypto | ripemd160+sha512 KATs, native `secp256k1`, bip32 root fp `73c5da0a`, sign/verify | PASS |
| PSBT parse | `PSBTParser` on regtest 2-of-3 p2wsh 3-input fixture (policy/amounts correct, 1.20 s) | PASS |
| PSBT sign | `psbt.sign_with(root)` → 3 sigs added, 652 ms | PASS |
| Camera | `camera_scanner` start → 8 s live preview session (scan UI renders, webcam-verified) → stop | PASS |
| i18n | `ru` pack staged from SD → `load_locale` → Cyrillic screen rendered (webcam-verified: "Настройки/Подпись/Кошелёк/Сид-фраза") | PASS |
| Touch | GT911 detected + initialized in boot log | PASS (init-level) |

Caveats: camera QR *decode* not exercised (nothing QR-like in the P4 camera's view — dark room; pipeline
start/stream/UI/stop all verified). Physical touch *taps* can't be automated — GT911 init + config verified.

**P1 status: COMPLETE** — static proof + runtime proof + regression on device. Commits `a85ffc0`
(strip), `b39c7e2` (board_common bump; submodule commit `2dc1f44` on `feat/radio-coproc-hold-in-reset`).

## Decisions made

1. **Screens submodule bumped to upstream main 267cc64** as branch's first commit (user merged screens PRs; keeps regression surface current). Build-verified.
2. Baseline flashed from the same branch/build that P1 modifies, so before/after diffs are apples-to-apples.
3. **Strip default-ON for all boards** (`MP_DISABLE_NETWORK=0` env rebuilds a networked debug image). Rationale: SeedSigner never uses networking on any target; S3 boards get the same treatment on their next build rather than a per-board gate.
4. **Replaced (not nursed) the broken parts of the old machinery**: kept the `_MICROPY_NET_OFF` source/component-list logic in `esp32_common.cmake` (sound), added env-var detection, moved exclusion to project level, deleted the dead `main/CMakeLists.txt` block. Brief explicitly authorized rework.
5. **tinyusb patched rather than tolerated**: compile-only network residue would have been harmless, but patching removes USB networking (RNDIS/NCM) at the source level — for an air-gapped signer, "not compiled" beats "compiled but disabled", and the patch is one hunk in a version-pinned component.
6. **C6 held in reset via GPIO54 without the board schematic in hand** (Waveshare wiki is Cloudflare-blocked; local KB has no P4-43 schematic PDF). Evidence chain: esp_hosted P4 preset pin + polarity, Waveshare's own wifi demo using stock esp_hosted defaults, GPIO54 unused in our board config. Device regression will catch any wiring surprise.

## Blockers / gotchas hit

- `authorize-git` writes its flag file to the shell's cwd — first authorization stranded a stale `.claude-auto-commit` in `deps/seedsigner-lvgl-screens/` (hook blocks moving/deleting it). **User: delete it manually.** Re-ran from repo root; gate live.

## Recommended next steps (if this run stops here)

- Continue P1 per the brief: probe `-DMICROPY_DISABLE_NETWORK=ON` build, remove mdns from `idf_component.yml`, add sdkconfig `=n` overrides.
