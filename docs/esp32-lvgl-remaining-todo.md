# TODO — remaining ESP32 firmware work for the SeedSigner LVGL cutover

**Created 2026-07-17.** Working-tree note. The screen/toast/scan/qr-display/address-verification
bindings (incl. `camera_entropy` + `set_labels`) are all present on `main` and firmware-validated;
submodule `deps/seedsigner-lvgl-screens @ 92be1e0` is current (no drift). This is the standing
"next priority" note for finishing the LVGL cutover on ESP32: the builder-side items below
(BUILDER-1, BUILDER-2) plus the app-side integration roadmap (carried in §"App-side LVGL
integration roadmap" from the main `seedsigner` repo hub). None of it is on the current frozen
firmware.

> **Current frozen firmware context (2026-07-17):** the k_quirc development tip now carries the
> finalized adaptive-threshold work — upstream **PR #11** (bootstrap sweep + lock, emissive/
> reflective media-profile ladders) plus the **blend gate** (`feat/blend-gate-bail`), with the
> gate default bumped to the frame-audited floor **170** per-mille. That is the QR-decode state
> baked into the new 4.3 / 3.5 builds; the LVGL items in this note are the work that comes next.

> **Hub:** `seedsigner/docs/_integration/pil-hardwarebuttons-retirement-master-todo.md`
> (BUILDER-N + SEC-1). Source: 2026-07-17 cross-repo audit. Details for BUILDER-1 also live in
> this repo's `docs/camera-entropy-overlay-todo.md` (2026-07-12 addendum). The §"App-side LVGL
> integration roadmap" below mirrors that hub so this repo is self-contained for the next cycle.

## BUILDER-1 — full-display confirm-image in the entropy overlay

The base `camera_entropy` overlay (PREVIEW → CAPTURING → CONFIRM with a center-square frozen
frame) + `set_labels` is done. Enhancement: on entering CONFIRM, show the captured image
**full-display, crop-to-fill + contrast-stretched** (faithful to the PIL
`ToolsImageEntropyFinalImageScreen` it replaces), using the screens APIs already in the pinned
submodule (`image_entropy_process()`, `camera_entropy_overlay_set_confirm_image()`, PR #73
`f948b1f`).

- In `ports/esp32/camera_entropy/camera_entropy.cpp`: on `capture()`/entering CONFIRM, run
  `image_entropy_process()` on the latched **RAW** frame → a PSRAM buffer → push via
  `camera_entropy_overlay_set_confirm_image()`. Auto-hide on `resume()`.
- **Invariant:** crop + contrast are **display-only**. `get_result()`'s `(chain, frame)` must stay
  the RAW latched bytes — never the processed display image.

## BUILDER-2 — expose device-uniqueness + time for entropy composition (SEC-1)

Gated on the **SEC-1 sign-off** (see hub): if the native image-entropy hash must re-add the
device-serial + time contributions the PIL path folds in, the app's native branch needs
`machine.unique_id()` + `time.ticks_ms()` (standard esp32 port surface).

**✅ Verified present (2026-07-17).** The P4-43 firmware ELF exports `machine_unique_id` /
`mp_machine_unique_id` (`MP_QSTR_unique_id`, backed by `ESP_EFUSE_OPTIONAL_UNIQUE_ID`) and
`time.ticks_ms` / `ticks_us` / `ticks_diff`; `MICROPY_PY_MACHINE` is `1` in the esp32
`mpconfigport.h` and the network strip does not touch the `machine`/`time` modules. So no
firmware change is needed for BUILDER-2 — the primitives are already available to the app's
native `generate_mnemonic_from_camera_entropy` branch. This item stays open only as the
app-side contract: it unblocks once **SEC-1** decides whether to re-add the device-id + time
contributions (if SEC-1 accepts the drop, BUILDER-2 closes as a no-op).

## App-side LVGL integration roadmap (from the main `seedsigner` repo hub)

The remaining work to *fully* integrate the LVGL screens is mostly in the app repo, not the
firmware. It is coordinated by the hub
`seedsigner/docs/_integration/pil-hardwarebuttons-retirement-master-todo.md` (branch
`integration/lvgl-mpy`). Mirrored here so the builder repo carries the next-cycle plan; the hub
governs if they disagree. The `seedsigner` repo is **edit-restricted** — these land there via a
feature branch + the user's PR, not from this session.

**Two milestones (the app's, not the firmware's):** (1) retire the PIL-era `HardwareButtons`
GPIO reader once every input-blocking PIL screen on the Pi is native; (2) remove PIL entirely
(much further out — still coupled to the Pi camera source + display pipeline).

App-side items (milestone 1), several of which directly affect ESP32 behaviour:

| # | Task | Why it matters to ESP32 |
|---|---|---|
| APP-1 | `SeedTranscribeSeedQRConfirmScanView` → native scan (give it `ScanView`'s capability-gated / `IS_MICROPYTHON` split; `seed_views.py:1702`) | **Currently broken on ESP32** — the unconditional PIL `ScanScreen` path hits NotYetImplemented on device. Highest-value fix; no deps (both scan substrates exist in firmware). |
| APP-3 | Exception recovery → native `ErrorView` on CPython (`controller.py`) | Load-bearing for retirement; Pi-only, but keeps the native/PIL split coherent. |
| APP-4 | Exit blanking → native `clear_screen` (`controller.py:442`) | Pi-only. |
| APP-5 | Pi image-entropy native (`tools_views.py`) | Pairs with **BUILDER-1** — same native confirm-image path; gated on SEC-1. |
| APP-2 / APP-6..10 | addr-verify Pi flip, IO-test driver, toast `activation_delay` vestige, input-mode proxy, retire `HardwareButtons`, PIL display teardown | Pi-side cutover; no ESP32 firmware dependency beyond bindings already shipped. |

**SEC-1 (cross-cutting decision, needs an owner).** The native image-entropy path hashes only
`sha256(chain ‖ latched_frame)`; the PIL path additionally folded in the CPU serial + wall time.
The dominant camera-frame chain is preserved, but device-serial + time are dropped. Decide: re-add
device-id (`machine.unique_id()` on ESP32 / CPU serial on Pi) + a time source, or accept the drop.
This gates APP-5 and is the trigger for **BUILDER-1** (do the display-only confirm image) and the
already-satisfied **BUILDER-2** (primitives are exposed — see above).

**Suggested sequence (from the hub):** APP-1 → (RASPI-3 →) APP-2 → APP-3 + APP-4 →
(RASPI-1 + SEC-1 →) BUILDER-1 + APP-5 → APP-6 → APP-7 → APP-9 → APP-10.

## Not in scope (recorded)

- **io_test** — intentionally **not bound** for ESP32 (`docs/lvgl-screen-binding-gap-todo.md`:
  "io_test still skipped (Pi-only)"). No action unless a P4 I/O test is ever wanted (would need a
  bespoke `io_test_screen` + `io_test_set_capture_state` binding, and the screen forces
  `INPUT_MODE_HARDWARE` without restore — problematic on the touch panel).
- Submodule pin is current at `92be1e0`; no bump needed.
