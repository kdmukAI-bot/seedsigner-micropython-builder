# TODO: precompile the app to `.mpy` in `deploy_app.py` (kill the on-device compile stall)

Status: **IMPLEMENTED + DEVICE-VALIDATED 2026-07-03** on branch `feat/deploy-mpy-precompile`
(off `main`), UNCOMMITTED. Clean all-`.mpy` deploy to the P4, pinned via `git archive` to the
committed integration tip `integration/lvgl-mpy` @ `a3a7ac2` (121/122 modules → `.mpy`; the lone
`@micropython.native` file `hardware/displays/st7789_mpy.py`, unused on ESP32, ships as source);
`verify: PASS` (130/130 files) and on-device `import seedsigner.controller` → `SMOKE_OK` cold from
bytecode. Owner: builder session. Cross-refs the seedsigner-side import-hygiene trims (done
separately in `seedsigner/`, on that same integration tip) and the push-hardening checklist in
`docs/deploy-push-failure-scenarios-todo.md`.

## Why

On the ESP32-P4, the **first** navigation to each heavy main-menu screen stalls ~2–4s, then that
screen is fast forever after. Root cause: `tools/deploy_app.py` pushes **raw `.py` source** to the
internal-flash VFS (`/lib/seedsigner/...`, `/lib/embit/...`), so MicroPython **lexes+parses+compiles
each large module from source at first import**. A 2300-line `seed_views.py` compiling on-device *is*
the 2–4s. On-device measurement confirmed the shape: two independent compile clusters —
`{Scan → scan_views + decode_qr}` and `{Seeds ⊂ Tools → seed_views + encode_qr + tools_views}` —
each paid once, `Settings` cheap (tiny module).

The decisive fix is to stop compiling on-device: ship precompiled bytecode.

## The change

Add an `mpy-cross` stage to `deploy_app.py` so the app tree is compiled to `.mpy` **before** the VFS
push, and push `.mpy` (not `.py`) to the device.

Touch points:
- `tools/deploy_app.py:291` — `push_tree(ser, SS_SRC, SS_DST, "seedsigner", ...)`
- `tools/deploy_app.py:293` — `push_tree(ser, EMBIT_SRC, EMBIT_DST, "embit", ...)` (embit compiles
  on-device too; do it as well for full benefit)
- `push_tree` currently excludes `.pyc` and walks `.py`. New flow: for each `.py`, run `mpy-cross`
  into a staging dir (or on the fly), then push the resulting `.mpy`. Push **only** `.mpy` for compiled
  modules (don't push both `.py` and `.mpy` for the same module — avoid ambiguity; the importer
  prefers `.mpy` anyway).
- Keep the module-pop-so-it-reimports logic (`deploy_app.py:101-104`) — it works the same with `.mpy`.
- The `secp256k1` dev shim written at `deploy_app.py:54` is a single tiny module; leave as `.py` or
  compile it too, either is fine.

Gotchas (all manageable):
- **Use the version-matched `mpy-cross`.** The `.mpy` ABI is pinned to the firmware's MicroPython
  version (this tree = **1.27.0**, `deps/micropython/upstream/py/mpconfig.h:41-43`). Build/use the
  `mpy-cross` from `deps/micropython/upstream/mpy-cross` (not a random `pip install mpy-cross`, which
  may mismatch). It is **not built yet** — add a build step or document the one-liner
  (`make -C deps/micropython/upstream/mpy-cross`).
- **No `-march` needed.** The app is pure Python; bytecode is architecture-independent. `-march` is
  only for `@native`/`@viper`/inline-asm, which the app doesn't use. Plain `mpy-cross` is correct.
- **Keep line numbers** (do **not** pass `-s`) so on-device tracebacks stay useful in dev.

## Dev-cycle impact: none meaningful

No firmware rebuild. The loop stays `edit .py → mpy-cross (host, ~ms) → push .mpy → reset & run`,
identical to today plus a fast host-side compile, and the push is smaller (so faster). This is the
whole reason `.mpy`-on-VFS is the right *dev* path.

## Release follow-on (separate task): freeze into signed firmware

The GC heap is PSRAM-backed with ~24 MB+ free, so **capacity is a non-issue** — the real cost of
resident `.mpy` modules is **PSRAM working-set + bus-bandwidth contention with the camera/display DMA**
(see this repo's `docs/camera-pipeline-*` and `consumer-psram-contention-vs-display.md`). For a release
build, freeze the app into the **signed firmware manifest** (`ports/esp32/boards/.../manifest.py`,
alongside the already-frozen `logging`/`hmac`): frozen bytecode is XIP'd from flash, stays off the
PSRAM heap (shrinks the working set / bus pressure) and is covered by secure boot. Dev keeps the fast
`.mpy`-on-VFS loop; release freezes. Both eliminate the on-device compile.

## Verify

`tools/deploy_app.py --mode run`, then click each of Scan / Seeds / Tools / Settings from the menu:
all first-clicks should be instant (no compile). Cross-check a Seeds xpub-export (UR), SeedQR display,
signed-message QR, and a multisig Verify-Address flow still work from the `.mpy` build.
