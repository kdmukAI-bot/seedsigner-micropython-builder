# UR PSBT scan shows "Not Yet Implemented" on device — `urtypes` is missing

**Date:** 2026-07-02
**Board:** Waveshare ESP32-P4 Touch LCD 4.3
**Status:** Root cause confirmed by static analysis. **Fix applied (Tier 2 — freeze into
firmware):** `urtypes==1.0.1` vendored to `deps/third-party/urtypes/` and frozen via the
P4 board `manifest.py` (`package("urtypes", base_path="$(MPY_DIR)/../../../deps/third-party")`).
Pending on-device validation after the next firmware rebuild + reflash. Tier 1 (the
`/lib` deploy-push stopgap) was intentionally **not** kept — frozen supersedes it, same
lifecycle as `logging`/`hmac`.

## Symptom

On the P4, with the camera pipeline wired into the real app, scanning a **fully-read
animated UR PSBT** (e.g. `btc-datagen/output/psbt_2of3_p2wsh_3in_low.gif` or
`..._normal.gif`, both `ur:crypto-psbt/...`) lands on the LVGL **"Work In Progress /
Not Yet Implemented"** notice instead of the expected `PSBTSelectSeedView`
("Select Signer") button list.

It *looks* like a screen that hasn't been migrated to LVGL. **It is not.**
`PSBTSelectSeedView` is already on the native LVGL button list and renders fine — it
is simply never reached.

## Root cause

`urtypes` (the third-party PyPI package that extracts the PSBT bytes out of the UR
CBOR) **is not present on the device**, so the extraction raises `ImportError`, and the
Controller's MicroPython exception fallback masks that as the "Not Yet Implemented"
notice.

Trace (all paths in the `seedsigner` repo unless noted):

1. Scan completes. `DecodeQR.detect_segment_type()` classifies the payload from its
   string prefix — `"ur:crypto-psbt/"` → `QRType.PSBT__UR2`. This is pure string ops
   and works identically on MicroPython. `is_psbt` is **True**.
   (`src/seedsigner/models/decode_qr.py:383`, `:287-294`)
2. `ScanView.run()` takes the PSBT branch and calls `self.decoder.get_psbt()`.
   (`src/seedsigner/views/scan_views.py`, the `elif self.decoder.is_psbt:` branch)
3. `get_psbt()` → `get_data_psbt()` → **`from urtypes.crypto import PSBT as UR_PSBT`**
   at `src/seedsigner/models/decode_qr.py:182`. On device this raises `ImportError`.
   The import is **outside** the `try/except` inside `get_psbt()`, so it propagates out
   of `ScanView.run()`.
4. The Controller main loop catches *any* exception from a View and, because the normal
   PIL `UnhandledExceptionView` can't render on MicroPython, routes to
   `NotYetImplementedView` as a "keep the session alive" recovery — after calling
   `print_exception(e)`. (`src/seedsigner/controller.py:373-385`)

So the `NotYetImplementedView` you see is the **Controller masking an `ImportError`**,
not the `scan_views` placeholder and not a missing button-list migration.

### Why `urtypes` is missing

At diagnosis time, `tools/deploy_app.py` only pushed `seedsigner` → `/lib/seedsigner`
and `embit` → `/lib/embit`, plus the `secp256k1` dev shim, with `logging`/`hmac` frozen
into firmware. `urtypes` was neither vendored to `/lib` nor frozen into the board
manifest — it existed only in the CPython `.venv` (`urtypes==1.0.1`, pinned in
`seedsigner/requirements.txt`). It is now frozen into firmware (see **Fix** below).

### Why it was non-obvious

- The QR **classification** works on device (string prefix match), so `is_psbt` is
  correctly True — the failure is one step later, in payload *extraction*.
- `scan_views` never explicitly routes a PSBT to `NotYetImplementedView`; the route
  comes from the Controller's catch-all, so grepping `scan_views` for the placeholder
  is a dead end.
- The real `ImportError` traceback **is already printed to serial** via
  `print_exception(e)` immediately before the notice — confirm there first.

This is **separate** from the "UR fountain plateaus at 99%" item parked in the previous
handoff (that was optics/focus; completion is `is_complete()`, the 0.99 cap is
cosmetic). That issue is about *finishing the scan*; this one is about *what happens
after it finishes*. Both had to be true to see this: you only hit the `urtypes`
`ImportError` once a UR PSBT actually completes.

## Fix — frozen into firmware (applied)

`urtypes==1.0.1` is vendored as a pinned in-repo copy at **`deps/third-party/urtypes/`**
and frozen into the P4 firmware via the board manifest
(`deps/micropython/mods/new_files/ports/esp32/boards/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43/manifest.py`):

```python
package("urtypes", base_path="$(MPY_DIR)/../../../deps/third-party")
```

- `$(MPY_DIR)` = `<repo>/deps/micropython/upstream`, so `../../../deps/third-party`
  resolves to `<repo>/deps/third-party`.
- **`require("urtypes")` does NOT work** — `require()` only resolves official
  micropython-lib names; `urtypes` is third-party PyPI, so it must be frozen from
  vendored source via `package()`.
- Freezing runs `mpy-cross` at build, which surfaces any syntax incompatibility. Low
  risk: `urtypes` is written to run on MicroPython (Foundation Devices' Passport uses
  it).
- Unlike embit — which still needs a native `secp256k1` build step and is vendored to
  `/lib` via `deploy_app.py` — `urtypes` is pure Python with no native deps, so it was
  frozen on its own now rather than waiting for the embit/secp256k1 freeze pass.

Requires a **firmware rebuild + reflash** (frozen modules are baked into the image); an
app-only `deploy_app.py` push does not pick it up.

Minor cleanup: the vendored copy carried `__pycache__/*.pyc` — harmless to `package()`
(it globs `.py`), but worth `.gitignore`-ing / removing so stale bytecode isn't
committed.

### Why not the `/lib` deploy-push stopgap

An earlier option was to vendor `urtypes` to `/lib/urtypes` via `deploy_app.py` (the way
embit is handled today) for a no-reflash unblock. It was intentionally **not** kept:
freezing supersedes it and puts `urtypes` on the same lifecycle as `logging`/`hmac`
(baked into firmware, nothing to vendor at deploy time).

## Verify the fix

1. Rebuild + reflash the P4 firmware (frozen `urtypes` only lands via a new image).
2. Rescan `psbt_2of3_p2wsh_3in_normal.gif` (4 dense parts) or `..._low.gif` (20 parts).
3. Expected: flow reaches **`PSBTSelectSeedView`** ("Select Signer") — with no seeds
   loaded it shows *Scan a seed / Enter 12-word / Enter 24-word*.
4. Watch serial: the `urtypes` `ImportError` should be gone.

## Next thing to watch

Tier 1 only gets you *past* the UR extraction. The very next on-device step is
`embit.psbt.PSBT.parse()` inside `get_psbt()` (`decode_qr.py:173`). If embit's PSBT
parse has its own MicroPython gap it'll surface as the *same* "Not Yet Implemented"
notice (same Controller catch-all) — so keep reading the serial traceback until you
actually land on the Select Signer screen.
