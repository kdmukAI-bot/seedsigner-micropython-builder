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

## Decisions made

1. **Screens submodule bumped to upstream main 267cc64** as branch's first commit (user merged screens PRs; keeps regression surface current). Build-verified.
2. Baseline flashed from the same branch/build that P1 modifies, so before/after diffs are apples-to-apples.

## Blockers / gotchas hit

- `authorize-git` writes its flag file to the shell's cwd — first authorization stranded a stale `.claude-auto-commit` in `deps/seedsigner-lvgl-screens/` (hook blocks moving/deleting it). **User: delete it manually.** Re-ran from repo root; gate live.

## Recommended next steps (if this run stops here)

- Continue P1 per the brief: probe `-DMICROPY_DISABLE_NETWORK=ON` build, remove mdns from `idf_component.yml`, add sdkconfig `=n` overrides.
