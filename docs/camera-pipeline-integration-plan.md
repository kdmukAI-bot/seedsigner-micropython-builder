# ESP Camera Pipeline → LVGL → MicroPython — Audit, Plan & Repo Ownership (ESP32-P4 4.3")

## Context
The ESP32-P4 camera pipeline is R&D code, paused when priorities shifted. Before building the
MicroPython/LVGL integration on it we audited whether it's a sound foundation, how its display approach
coexists with our LVGL screens, and what others have done about the same roadblocks. **End goal:** prove the
round-trip `main_menu` (LVGL) → tap *Scan* → camera pipeline → decode a QR → control back to MicroPython →
result screen (LVGL). This doc = the audit verdict + a de-risked plan + **which repo owns which work** +
**what docs to write where**.

## Audit verdict
- **Engine: sound — build on it.** Triple-buffer + atomic promotion + drain-on-destroy, injected
  camera/display vtables, PPA scale, k_quirc consumer. Caveats are "v0.5," not structural (PPA math
  converged; single-consumer hard-coded; k_quirc vendored).
- **Display-compositing was the only interim part — and its hard version is descoped.** Full-screen preview
  was the unsolved frontier; **it is NOT a goal.** The **landscape square preview** (camera square filling
  the 480 px short dimension) IS the target and is already proven.
- **One real ceiling: PSRAM bandwidth.** The cheapest, highest-ROI lever is reducing PSRAM traffic.

## Established facts (reference)

**Boards.** 4.3" (DSI) = *performance*-bound, the POC target. 3.5" (SPI) = *architectural*-bound
(shared bus → dummy-draw; complexity "irreducible"); eventual support with accepted low fps; **not in this
POC.**

**Measured preview baselines** (notes, 2026-04 — the proven ≈10 fps was the *heaviest* sensor mode under
max contention, i.e. a **floor, not a ceiling**):

| Config | Display fps | Note |
|---|---|---|
| **4.3" DSI landscape (CPU-rotate)** | **≈10 fps** | the target path; full-frame flush rotation |
| 4.3" DSI landscape (PPA-rotate) | 2 fps | failed experiment, reverted |
| 4.3" DSI portrait | ≈15 fps | wrong orientation — not the target |
| 3.5" SPI portrait / landscape | ≈10 / 9.8 fps | dummy-draw (camera writes panel directly) |
| 4.3" quirc decode | 8–13 fps | fast; **preview, not quirc, is the concern** |

**Hardware constraints.**
- **PSRAM ≈0.8–1.0 GB/s, shared** by CSI+ISP+PPA+DSI-scanout+QR-read (Espressif: "PPA performance highly
  relies on PSRAM bandwidth"). The dominant per-frame cost — the ≈55–60 ms camera PPA pass — is
  contention-inflated, not compute.
- **Single PPA SRM engine** → never rotate twice (camera-scale + display-rotate serialize → the 2 fps).
- **DSI panels have no hardware rotation** — every rotation is PPA/CPU before scanout.
- **OV5647 = 5 fixed RAW modes** (RAW8 800×640 / 800×800 / 800×1280 @50; RAW10 1280×960-binning @45;
  RAW10 1920×1080 @30). No arbitrary square — capture a mode, PPA-downscale. **Prefer RAW8 800×640** (the
  PPA crops to 640×640 + scales free; lowest upstream traffic).
- **PPA can output YUV420 with a contiguous Y plane** (confirmed in ESP-IDF HAL); ISP/esp_video also expose
  GREY/NV12/YUV420/YUV422P. (No YUV422 on current P4 silicon.)

**`esp_lvgl_port`** (the least-understood layer): a thin LVGL↔`esp_lcd` glue — runs the **LVGL task**, owns
the **lock**, feeds the **tick**, **wires display+touch** (and the DPI framebuffers + tearing flags
`direct_mode`/`avoid_tearing`/`full_refresh`/`sw_rotate`), and exposes **`lvgl_port_stop()`/`resume()`**
(the dummy-draw hook). `esp_lvgl_adapter` was the alternative (TRIPLE_PARTIAL rotation+anti-tearing); it
caused 4.3" boot crashes, so board_common **reverted to `esp_lvgl_port` v2.7.2** + a custom landscape flush.
**Path B needs no fork** — `stop`/`resume` is the hand-off hook.

## Locked decisions
- **Board:** 4.3" (DSI). 3.5" deferred (low fps accepted).
- **Preview:** **landscape square** filling the 480 px short dimension — **NOT full-screen.** Low-res
  processing upscaled to fill is fine.
- **QR corpus & worst case (corrected — supersedes the old "v10–13/<70 modules" estimate):** the real
  Sparrow corpus spans **UR animated (81–85 modules / v16–17)**, static multisig descriptors (**2-of-3 = 77
  / v15**, **3-of-5 = 93 / v19**), and **BBQr (89–125 modules / v18–27)**. The **must-scan baseline is the
  large-data animated UR = 85 modules / v17.** At ≈3 px/module + 90% fill ⇒ **≈320² default-mode decode
  floor** (not 240²). Drives a **two-mode hypothesis**: a *default* mode for UR animated (≤85) and an
  *opt-in high-resolution* mode for BBQr (89–125) + static descriptors. **`K_QUIRC_MAX_VERSION` (default 25)
  must rise to ≥27 to decode BBQr-max (v27).** Full detail + benchmark spec:
  `../esp-board-common/docs/qr-scanning-performance-requirements.md`.
- **POC QR handling:** **raw payload only** (no DecodeQR on device).
- **POC command & control:** a **standalone harness** (`tools/*.py`), not seedsigner's ScanView.
- **board_common pin:** bump `490f027` → `ae94bf6` (carries the camera adapters on `esp_lvgl_port` v2.7.2).

## Stage leads — one session per stage, committing across the repos it touches
Coupled changes (e.g. an engine tweak in esp-camera-pipeline + its adapter in esp-board-common) are done by
**one lead session**, anchored where that stage's iteration/validation happens, with authority to commit to
every repo it reaches into. **Do not split a coupled change across independent sessions.** Docs still land in
whichever repo the *finding* belongs to — the lead session writes them there (see the doc table).

| Stage | **Lead** (anchor here) | Reaches into | Why this lead |
|---|---|---|---|
| **Phase 1** — perf spike, compositing, overlay rendering | **esp-board-common** | esp-camera-pipeline (engine: PPA output format/geometry, debug stats); seedsigner-lvgl-screens (overlay spec) | the `qr_decoder` app is the build/flash/profile vehicle; adapters + display + overlay code live here, and engine tweaks only make sense validated against it |
| **Phase 2** — MicroPython integration POC | **seedsigner-micropython-builder** | board_common (pin bump) + esp-camera-pipeline (submodule add); esp-board-common only if an adapter change surfaces | it orchestrates the firmware build + owns camera_manager/binding/harness; adapters are settled by Phase 1 |
| **Later** — production scan abstraction + Pi Zero overlay reuse | **seedsigner** | seedsigner-lvgl-screens; seedsigner-raspi-lvgl | the seam is business-logic-shaped (`DecodeQR`/`ScanView`) |

## Phase 1 — R&D & performance spike   ·   lead: **esp-board-common** (reaches into esp-camera-pipeline + lvgl-screens)
Run in the standalone `esp-board-common/apps/qr_decoder` app (camera + adapters + QR + overlays + debug
stats already integrated; no MicroPython). **Goal: settle the display config with real numbers.**

1. **Decode-resolution floor.** Sweep **PPA output square** ({280, 320, 360}, bracket with 240/480) ×
   **QR-density** against the real fixtures (must-scan baseline = **85-module / v17** large-data UR; static
   77 & 93 in `apps/qr_decoder/test_qrs/` bracket it). Optionally sweep **sensor mode** (RAW8 800×640 vs
   1280×960-binning) if the square sweep can't clear the baseline. quirc runs on the (small) PPA output
   square, so the square is the **master knob** (smaller = faster quirc + less PSRAM, bounded by ≥3
   px/module @ 90% fill). Measure decode reliability (`ok%`/`id%`) + quirc time + decode fps via the app's
   serial stats + HUD. Confirm `K_QUIRC_MAX_VERSION` clears the target (≥27 for BBQr-max). Expected
   default-mode sweet spot **≈320²** (not 240²). **Default-mode go/no-go: smallest square that reliably
   decodes 85 modules *and* sustains ≥~10 decode fps on a live UR animation (Sparrow default 5 fps).** Full
   requirements + benchmark spec: `../esp-board-common/docs/qr-scanning-performance-requirements.md`.
2. **Preview pixel format.** Default **PPA → display-native RGB565** (preview just blits; quirc does its own
   cheap small-image RGB→gray — preview is the bottleneck, not quirc). *Reserve:* PPA→YUV420 (quirc reads
   the contiguous Y free, display does YUV→RGB) only if quirc ever becomes the bottleneck.
3. **Preview fps — order by ROI:**
   - **Lever 1 (cheap, do first, no re-architecture):** lighter sensor mode + smaller square + existing
     demand-frame-skip, within the current Path A (LVGL-landscape + full-frame CPU flush rotation + plain
     LVGL overlay widgets). Attacks the dominant ≈60 ms PPA pass / PSRAM contention. The proven 10 fps was
     the worst-case mode, so this likely reaches "acceptable" alone.
   - **Lever 2 (escalation, only if still choppy): DSI dummy-draw via `lvgl_port_stop()`.** Halt LVGL;
     camera writes the DPI FB **pre-rotated** (free, camera PPA); overlays are **LVGL-rendered to an
     off-screen `lv_canvas`** (works with the task stopped) and **composited manually** — the proven DSI
     pattern; `lvgl_port_resume()` for menu/result. No fork. Removes the ≈22 ms full-frame rotation (worth a
     bit more than its 22% via lock-hold + 1.5 MB/frame PSRAM) but it's not the dominant cost → modest win
     vs. real effort. The 3.5"'s dummy-draw (≈10 fps on slow SPI) is the working proof.
4. **Output:** a settled, measured config (sensor mode, processing square, pixel format, preview path,
   achieved fps) → go/no-go for integration.

## Overlay definition   ·   spec home: **seedsigner-lvgl-screens** (built during Phase 1, esp-board-common-led)
- Define overlays as a **declarative zone/spec** (status text, headline, framing guide, **animated-QR
  progress**) — *leveraged differently* from full screens. **Split definition from rendering.**
- Rendered ≥3 ways from one spec: ESP Path A (LVGL widgets) · ESP Path B (manual composite of an
  LVGL-rendered canvas) · Pi Zero later (PIL or CPython-LVGL). Keep ESP-specific rendering (rotation,
  stride blits, dummy-draw) *out* of the spec. This is the "Unified Display Zone Renderer" from
  `text-overlay-architecture.md`, elevated to the shared repo.
- **Complexity driver:** the **animated-QR status indicator** (fill bar + percent advancing as segments
  arrive) → the spec needs *parameterized/updatable* content, updated on decode-progress events (a few/sec,
  not per-frame); its data comes from `DecodeQR` (the `CameraScanner` data-side seam).
- **Now:** only avoid foreclosing it; the POC overlay can be minimal but spec-defined, not hardcoded.

## Phase 2 — MicroPython integration POC   ·   lead: **seedsigner-micropython-builder**
1. **Bump `board_common` → `ae94bf6`, re-validate existing firmware FIRST** (22-commit drift incl. the
   esp_lvgl_port round-trip): build the 4.3" and confirm menus, Approach A fonts, i18n still pass on device.
2. **Build wiring:** add `deps/esp-camera-pipeline` submodule (pin `5fb3fa2`); extend
   `MICROPY_EXTRA_COMPONENT_DIRS` in `scripts/build_firmware.sh`; CSI/ISP/OV5647 in the 4.3" `sdkconfig.board`
   (reconcile WDT vs blocking DQBUF).
3. **`camera_manager`** (`ports/esp32/camera_manager/`): rebuild from the prior version, **strip overlay
   code** (≈538→≈180 lines); keep lifecycle + the non-blocking **QR ring buffer + `qr_poll`**; apply the
   spike's config via `board_pipeline_default_config()`; keep the blank-screen-before-delete `act_scr` guard.
4. **Binding** (`bindings/modcamera_manager_bindings.c`, header-only for QSTR scan): `pipeline_create/
   destroy`, `qr_create/destroy`, `qr_poll()->bytes|None`, `qr_clear_queue`; register in
   `bindings/micropython.cmake`.
5. **Harness** `tools/poc_scan_qr.py`: `main_menu_screen` → poll for *Scan* → `pipeline_create`+`qr_create`
   → poll `qr_poll()` → on payload tear down → `large_icon_status_screen` with `len(payload)` + hex preview.

## Documentation to write (per repo)
| Repo | Docs |
|---|---|
| **esp-camera-pipeline** | `docs/`: engine perf profile; PPA output-format (RGB565 vs YUV420) decision + contiguous-Y finding |
| **esp-board-common** | `docs/knowledge/`: UPDATE `p4-lcd43-landscape-pipeline-optimization.md` with Lever-1 (sensor-mode/square) + Path-B(DSI dummy-draw) results; new DSI-dummy-draw doc; resolution-vs-decode-reliability; (begin "Unified Display Zone Renderer" extraction) |
| **seedsigner-micropython-builder** | `docs/knowledge/`: `camera_manager`↔MicroPython contract; menu→scan→result handoff + `act_scr` guard; build/submodule + sdkconfig (CSI/ISP/WDT) notes |
| **seedsigner-lvgl-screens** | `docs/`: overlay zone/spec definition + "leveraged differently" usage |
| **seedsigner** (later) | `CameraScanner` abstraction design |

## Risks & open questions
- k_quirc `MAX_VERSION` must clear the worst-case QR version at the chosen resolution.
- Pixel-format reserve: YUV420→RGB on the display path must not need a 2nd PPA pass.
- board_common 22-commit drift must not regress menus/fonts/i18n (`esp_lvgl_port` v2.7.2 is the friction).
- Path B: the `lvgl_port_stop` ↔ DPI-FB handoff must be clean (targeted patch, not a fork, is the fallback).
- Camera-screen ownership (display adapter vs `camera_manager`); WDT vs blocking `VIDIOC_DQBUF`.

## Verification
Use the **esp-build** skill. **Phase 1** (esp-board-common): flash `qr_decoder` on the 4.3", run the
sensor-mode × square sweep, record fps from debug stats, scan the worst-case multisig QR. **Phase 2**
(builder): build+flash firmware, deploy `tools/poc_scan_qr.py`, observe menu → live square preview → result
screen; no WDT panic; stable REPL after teardown; decoded byte length matches the known payload.

## Deferred / NOT goals
- **Full-screen preview** — not a goal (the landscape *square* is).
- Production `CameraScanner` abstraction in seedsigner's real `ScanView`/`Controller` (shares the
  decoded-payload path; pairs with the overlay-spec UI path).
- Full `DecodeQR` format routing on-device.
- **3.5" (SPI) port** — eventual, low fps accepted; lever when it resumes: **grayscale preview** (ISP Y8
  halves SPI bytes ≈2× preview). Bottleneck is SPI transfer (physical), which layer separation can't fix.
- Engine hardening (single-consumer, k_quirc submoduling) unless the spike surfaces a blocker.
