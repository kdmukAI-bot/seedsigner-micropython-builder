# TODO: expose the LVGL MicroPython module as `seedsigner_lvgl_screens`

**Status:** pending — flagged 2026-06-20 from the seedsigner Foundation A work.

The seedsigner Python app now imports the LVGL screens C module as
**`seedsigner_lvgl_screens`** (previously referred to as `seedsigner_lvgl`). The
MicroPython C module this repo builds must be **registered/exposed under that
name** so `import seedsigner_lvgl_screens` works on-device.

- **What to change:** the MicroPython module registration — the `MP_REGISTER_MODULE`
  name in `bindings/modseedsigner_bindings.c` and anything in `usercmodule.cmake`
  that pins the module name.
- **Why:** a single, content-descriptive public import name across Pi + ESP32,
  matching the `seedsigner-lvgl-screens` source repo. It replaces the ambiguous
  `seedsigner_lvgl` and avoids the "adapter" label (which collides with ESP-IDF's
  `esp_lvgl_port` and a discarded "LVGL adapter" component).
- **Consumer:** seedsigner `src/seedsigner/gui/lvgl_screen_runner.py` does
  `import seedsigner_lvgl_screens as lv`.
- **Full rationale + the unified host-API contract:** see the seedsigner repo's
  `docs/architecture/lvgl-host-api-unification.md`.

Only the **import name** must be `seedsigner_lvgl_screens`; the underlying C
symbol is an implementation detail.
