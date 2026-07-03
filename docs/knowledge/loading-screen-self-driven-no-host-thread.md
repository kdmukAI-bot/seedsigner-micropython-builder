# Loading screen: self-driven animation replaces Python's LoadingScreenThread

## The question

SeedSigner's PIL/CPython loading spinner (`LoadingScreenThread`, `gui/screens/screen.py`)
is a background **thread**: the main thread runs a long blocking task (PSBT parse,
seed generation, change verification) while the thread repaints the orbiting comet,
and a **stop event** (`.stop()` → `keep_running = False`) ends it when the task is done.

How should that thread-plus-stop-event model be implemented on the ESP32
(MicroPython) firmware?

## The answer: no thread, no stop event

It isn't a thread on the LVGL backend at all. The native `loading_screen()`
(`components/seedsigner/seedsigner.cpp`) is a **pure builder** that installs a
self-driven `lv_timer`, and that timer runs on the `esp_lvgl_port` FreeRTOS task —
the *same* task that already pumps `lv_timer_handler` and renders every other screen.

So the three phases of the PIL model collapse:

| PIL / CPython (single render thread)                    | LVGL / ESP32 (dedicated display task)                          |
|---------------------------------------------------------|----------------------------------------------------------------|
| `LoadingScreenThread(...).start()` spawns a repaint thread | `seedsigner_lvgl_screens.loading_screen({"text": ...})` — builds the spinner, **returns immediately** |
| main thread runs the long task; the thread repaints     | MicroPython thread runs the long task; the **LVGL port task** repaints the comet autonomously |
| `.stop()` sets the stop event, thread exits             | host runs the **next screen**; `load_screen_and_cleanup_previous()` deletes the loading screen, whose `LV_EVENT_DELETE` handler tears down the spin timer |

Why the thread exists on PIL but not here: on the PIL backend a single thread can't
both do the work and repaint, so a *second* thread does the repainting and a stop
event coordinates the two. On both LVGL backends (ESP32 **and** Pi Zero) a dedicated
display task already repaints continuously, so the repaint thread — and therefore the
stop event — is unnecessary. "Stop the loading screen" degenerates into "load the next
screen," which every View already does.

## Why the animation keeps moving while the host is busy

The spinner is driven by `loading_spin_timer_cb` on an `lv_timer`, not by the host.
The MicroPython VM thread and the `esp_lvgl_port` task are **separate FreeRTOS tasks**,
so while the VM thread blocks inside its long task the port task keeps ticking and the
comet keeps turning. Critically, `run_screen()` (`display_manager.cpp`) takes the
LVGL-port lock only for the duration of the *build* and releases it before returning —
the long task then runs with the lock free, so it never blocks the render task.

**Caveat:** the comet advances only while the port task gets CPU. A task that never
yields the scheduler (a tight, lock-free C loop) can starve it. The spin integrator is
wall-clock-clamped, so under starvation it visibly *slows* rather than jumping ahead,
then recovers — a natural "still working" signal. Ordinary Python-level long tasks
yield between bytecodes and animate smoothly.

## Binding + host contract

- Binding: `seedsigner_lvgl_screens.loading_screen(cfg={"text": "..."})`
  (`bindings/modseedsigner_bindings.c`). Same `run_cfg_screen` shape as the other
  dict-config screens; `text` is optional.
- **Do NOT** run it through `run_lvgl_screen()` / the `poll_for_result` loop. It
  produces no terminal event, so that loop would spin forever. The host calls it
  **fire-and-forget**: build it, run the blocking task on the MicroPython thread, then
  load the next screen (which cleans it up). A thin `run_loading_screen(text)` helper
  in `lvgl_screen_runner.py` that just calls `_lv.loading_screen(cfg)` and returns is
  the right seam; `LoadingScreenThread.start()` maps to that call and `.stop()` becomes
  a no-op (the next `View.run_screen(...)` performs the teardown).

## Provenance

- Native screen added in `seedsigner-lvgl-screens` `f6ff18f`; depends on
  `btc_logo_for_active_profile()` (baked logo + display-profile selector) from
  `a6632ad`. Both land together in the submodule bump `aedea86 → 335a287`.
- The submodule bump also carries `seed_finalize_screen` (`298afcc`, not yet bound)
  and a mnemonic-entry defer fix (`f90a32c`).
