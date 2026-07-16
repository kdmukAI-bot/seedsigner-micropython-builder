# Toast overlay — Python binding contract (`show_toast` / `dismiss_toast`)

**Status:** authoritative contract for the native toast binding this repo must expose.
The SeedSigner app-side landed on `integration/lvgl-mpy` (2026-07-15); the native overlay
shipped in `seedsigner-lvgl-screens` (PR #74 @ `9dd91b1`, already in the pinned submodule).
Binding not yet implemented here (tracked in `docs/lvgl-screen-binding-gap-todo.md` §B).

This binding is **platform-symmetric**: `seedsigner-micropython-builder` (this repo, ESP32
MicroPython C module) and `seedsigner-raspi-lvgl` (Pi Zero CPython `.so`) must expose it
**identically** — same function names, same cfg schema, same semantics — so the shared
`seedsigner` app calls it with **no platform branch**. The identical contract also lives in
the raspi repo at `docs/toast-binding-contract.md`; keep the two in sync.

---

## Why a toast is not a screen

A toast is a transient banner pinned to the bottom of the display, built on the LVGL **top
layer** so it composites over whatever screen is live and survives screen swaps. It is
**not** a `run_cfg_screen`: there is no JSON-cfg path, no result event, no return value, and
it can be raised from a background producer task. So it is bound as a plain **fire-and-forget
push** — a bespoke binding pair, not a `run_cfg_screen` stanza (the C API takes a struct, not
a JSON `void*`).

`toast_overlay_screen()` in the screens repo is **desktop-tooling host only** — do not bind
it. On device the toast is raised over the live screen via the overlay manager.

---

## The Python surface (identical on both platforms)

```python
seedsigner_lvgl_screens.show_toast(cfg: dict) -> None     # raise or replace a toast
seedsigner_lvgl_screens.dismiss_toast() -> None           # dismiss the current toast, if any
```

> **Naming is normative — `show_toast` / `dismiss_toast` (verb-first).** The app calls
> `_lv.show_toast(cfg)` / `_lv.dismiss_toast()`. **This supersedes the `toast_show` /
> `toast_dismiss` sketch in `docs/lvgl-screen-binding-gap-todo.md` §B** — use the verb-first
> names so the shared app needs no platform branch (they must match the Pi Zero `.so`).

### `show_toast(cfg)` — cfg schema

The cfg is a plain dict (parse it directly into the struct — **not** via
`run_cfg_screen`/JSON). It is **policy-free**: the app resolves severity → glyph + colors
(Python's `Info`/`Success`/`Warning`/… toast subclasses) and passes the finished values; the
library renders exactly what it is told.

| key | type | required | default | meaning |
|---|---|---|---|---|
| `label_text` | `str` | **yes** | — | banner text; may contain `\n` for explicit line breaks (long lines also soft-wrap) |
| `icon` | `str` | no | *(none → text-only)* | a seedsigner-icon-font **PUA glyph** (e.g. `""`), i.e. a `SeedSignerIconConstants` value — the same glyph form button `icon`s use. Maps to the struct's `icon_glyph`. Omitted for an icon-less banner |
| `outline_color` | `int` | no | `0xFFFFFF` | `0xRRGGBB` — banner outline **and** icon color |
| `font_color` | `int` | no | `0xFFFFFF` | `0xRRGGBB` — message text color |
| `duration_ms` | `int` | no | `3000` | auto-dismiss delay in ms; **`0` = stay until dismissed/replaced** |

Colors cross the boundary as **`0xRRGGBB` ints** (a direct `uint32_t` for the native spec —
read them with `mp_obj_get_int`), **not** hex strings. Absent optional keys are **omitted**
by the app; apply the defaults above when a key is missing.

In practice the app always sends `label_text`, `outline_color`, `font_color`, and
`duration_ms`; `icon` is present only for iconed toasts. The defaults exist for robustness,
not as a normal path.

### `dismiss_toast()`

Dismiss the currently-showing toast immediately (no-op if none). **LVGL-thread only** — see
Threading. The app does not call this from its producer threads; it is here for a future
LVGL-thread caller.

---

## Semantics the native overlay owns (do NOT reimplement in the binding or app)

- **Auto-dismiss** after `duration_ms` (`0` = persist until replaced/dismissed).
- **Input dismissal.** The ESP32 boards are touch: a tap on the banner or a swipe-fling
  across it dismisses. (Hardware key/joystick dismissal also exists for keypad builds.)
- **One at a time** — a newer toast replaces the current one (Python runs a single toast).
- **Screensaver coexistence** — showing a toast dismisses the screensaver if it is up
  ("new toasts break out of the screensaver"), and suppresses screensaver activation while
  the toast is showing. (Owned by `overlay_manager`; the app's `Controller` no longer
  coordinates this.)

---

## Threading (the critical constraint)

- **`show_toast` must be safe to call from any producer task.** Route it through
  `overlay_manager_show_toast()`, which **stages** the request and drains it under the LVGL
  task's `lv_timer_handler` — never build widgets on the caller's task. If more than one
  non-LVGL task can stage concurrently (e.g. an SD-detect task alongside the main flow),
  override the weak `overlay_manager_lock/unlock` hooks with a FreeRTOS mutex so the staging
  buffer is safe.
- **`dismiss_toast` is LVGL-thread only.** It wraps `toast_overlay_dismiss()`, which mutates
  the widget tree immediately. There is **no** thread-safe `overlay_manager_dismiss_toast()`
  today. The app therefore never dismisses cross-thread — a toast that should disappear
  "early" instead relies on `duration_ms` expiry or being replaced. If a thread-safe dismiss
  is ever required, add a marshalled `overlay_manager_dismiss_toast()` in
  `seedsigner-lvgl-screens` first, then relax this line.

> Priority note: bind this pair only if/when the ESP32 app actually runs toast-producer
> paths (lower urgency than the passthrough screen stanzas in the binding-gap TODO §A). The
> contract is fixed regardless of when you implement it.

---

## Native C API wrapped (`seedsigner-lvgl-screens`: `toast_overlay.h` / `overlay_manager.h`)

```c
typedef struct {
    const char *label_text;    // required; may contain '\n'
    const char *icon_glyph;    // seedsigner-icon PUA glyph, or NULL for text-only
    uint32_t    outline_color; // 0xRRGGBB — banner outline + icon color
    uint32_t    font_color;    // 0xRRGGBB — message text color
    uint32_t    duration_ms;   // auto-dismiss delay; 0 = stay until dismissed/replaced
} toast_overlay_spec_t;

void overlay_manager_show_toast(const toast_overlay_spec_t *spec); // thread-safe; staged, drained on the LVGL loop
void toast_overlay_show(const toast_overlay_spec_t *spec);         // LVGL-task only, immediate (desktop tooling)
void toast_overlay_dismiss(void);                                  // LVGL-task only
bool toast_overlay_is_active(void);
```

The spec's strings are only read during the call (`overlay_manager_show_toast` deep-copies
what it needs), so a binding may build the struct on the stack from the cfg dict.

---

## App-side caller (source of truth)

- `seedsigner/src/seedsigner/gui/lvgl_screen_runner.py` — `show_toast()` builds the cfg dict
  (icon glyph passed through as `icon`; PIL colors mapped to `0xRRGGBB` ints; `None` fields
  omitted) and calls `_lv.show_toast(cfg)`. `dismiss_toast()` wraps `_lv.dismiss_toast()`.
  Both no-op when the native module is absent (host/CI).
- `seedsigner/src/seedsigner/gui/toast.py` — the severity classes
  (`RemoveSDCardToastManagerThread`, `SDCardStateChangeToastManagerThread`, `InfoToast`,
  `SuccessToast`, `WarningToast`, `DireWarningToast`, `ErrorToast`) hold the policy
  (label / icon glyph / colors / duration) and push it via the runner seam.

---

## Implementation in this repo (ESP32 MicroPython)

Wiring detail is in `docs/lvgl-screen-binding-gap-todo.md` §B and the working note
`docs/toast-overlay-integration-todo.md`. In short:

1. The pinned submodule `deps/seedsigner-lvgl-screens` already includes `toast_overlay.cpp`
   (its `CMakeLists.txt` lists it), and `overlay_manager_init()` is already called at boot
   (`ports/esp32/display_manager/display_manager.cpp:156`), so staged toast requests already
   drain — no build-graph edit.
2. Add a locked wrapper in `ports/esp32/display_manager/display_manager.{h,cpp}`:
   `display_manager_show_toast(...)` + `display_manager_dismiss_toast()` that take the
   `esp_lvgl_port` lock (mirroring the `set_screensaver_timeout` wrapper) and forward to the
   native toast API. Include `toast_overlay.h` / `overlay_manager.h`.
3. Add the MicroPython functions in `bindings/modseedsigner_bindings.c`:
   - `mp_seedsigner_lvgl_show_toast` — parse the cfg dict into a `toast_overlay_spec_t`
     (`label_text` str; `icon` str glyph → `icon_glyph`, NULL if absent; `outline_color` /
     `font_color` / `duration_ms` ints) → `display_manager_show_toast` →
     `overlay_manager_show_toast` (staging; thread-safe).
   - `mp_seedsigner_lvgl_dismiss_toast` → `display_manager_dismiss_toast` →
     `toast_overlay_dismiss`.
   Use the existing `MP_DEFINE_CONST_FUN_OBJ_*` pattern and add both to
   `seedsigner_lvgl_module_globals_table` (~L909–946, alongside `screensaver_screen`).
   **Export them as `show_toast` / `dismiss_toast`** (`MP_QSTR_show_toast` /
   `MP_QSTR_dismiss_toast`) — NOT `toast_show` / `toast_dismiss`.
4. **Thread-safety:** if an SD-detect (or other) task can stage a toast concurrently with the
   main flow, override the weak `overlay_manager_lock/unlock` with a FreeRTOS mutex.

---

## Verification (once bound + rebuilt)

Flash a P4 board and confirm: each severity toast renders with the right icon + colors over a
live LVGL screen; auto-dismiss after `duration_ms`; a tap / swipe-fling dismisses; a toast
raised during the screensaver breaks it. (Toast-producer paths on ESP32 may be limited today
— at minimum smoke-test via a direct `seedsigner_lvgl_screens.show_toast({...})` call.)
