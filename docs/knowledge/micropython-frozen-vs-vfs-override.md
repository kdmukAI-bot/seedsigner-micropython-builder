# Overriding a frozen app module from the flash filesystem (MicroPython, ESP32)

How a `.py` written to the ESP32 flash VFS can (or can't) shadow a module that was frozen into the
firmware via the manifest. This governs both the frozen-app design and any "push updated code to
flash without a full rebuild" dev path. Verified against MicroPython v1.27.0 as built here.

## The one-line answer

**`sys.path` at boot is `['', '.frozen', '/lib']`.** Precedence is purely this order — frozen is
**not** special-cased or checked first. So:

- A package at the **VFS root** (`/seedsigner`) — entry `''`, index 0 — **shadows the frozen copy.**
- The **frozen** copy — the `.frozen` entry, index 1 — wins only if the root had no match.
- **`/lib` is dead on a frozen build** — entry index 2, *after* `.frozen`, so it's never reached
  when a frozen copy exists. (This is why `tools/deploy_app.py`, which pushes to `/lib/seedsigner`,
  is silently inert on a firmware that froze the app: its pushes are shadowed, and its
  import-smoke/run modes exercise the *frozen* code, not what was pushed.)

Net precedence: **VFS-root copy > frozen > /lib copy.**

## Why (mechanism)

- `sys.path` is assembled in two stages: core default appends `''` then `'.frozen'`
  (`deps/micropython/upstream/py/runtime.c:144-148`), then the esp32 port appends `'/lib'`
  (`deps/micropython/upstream/ports/esp32/main.c:141`). `''` resolves against the VFS current dir,
  which is `/` at boot (the flash FS is mounted at `/` by the frozen `_boot.py`).
- Import iterates `sys.path` in order and takes the **first hit**
  (`py/builtinimport.c` `stat_top_level`). The fork between frozen and VFS is by path *string*: a
  candidate beginning `".frozen/"` routes to `mp_find_frozen_module()`; anything else routes to the
  VFS (`py/builtinimport.c:60-71`). Frozen resolution is thus tied to the **position** of the
  `.frozen` entry, not to any early check.

## The hard constraint: override granularity = whole top-level package

You **cannot** override a single file of a frozen package. When a package is resolved, its
`__path__` is stored as the exact matched location string (`py/builtinimport.c:500`); submodule
imports do **not** re-scan `sys.path` — they build `"<parent __path__>/<child>"`
(`py/builtinimport.c:436-445`). So once `seedsigner` resolves to `.frozen/seedsigner`, every
submodule (`seedsigner.views.view`, …) is pinned inside the frozen image; a stray
`/seedsigner/views/view.py` is never consulted. **To override, you must shadow the entire top-level
package** at a `sys.path` entry that precedes `.frozen`. You *can* override just `seedsigner` and
leave `embit`/`urtypes` frozen (each is its own top-level name). Single-file `module("foo.py")`
freezes are independently shadowable for the same reason (each is its own top-level name).

## Corollary: frozen content is import-only, never `open()`-able

`package()`/`module()`/`freeze_as_str()` accept only `.py`
(`deps/micropython/upstream/tools/manifestfile.py:482,498-500,553-558`), and frozen content is
reached exclusively through the import machinery (`py/builtinimport.c` → `mp_find_frozen_module`),
never the VFS `open()` path. (The open-able ROMFS VFS exists but is disabled here —
`MICROPY_VFS_ROM=0`.) So **build metadata baked into the firmware must be a frozen `.py` module**
(the `seedsigner_frozen_build.py` marker pattern), not a data file like `version.json`. A version
`.py` placed *inside* the `seedsigner` package travels with whichever copy of the package wins
(frozen or a flash overlay) — a top-level version module would not.

## Boot exec order: only `_boot.py` is frozen-run; `boot.py`/`main.py` are VFS

The esp32 startup sequence (`deps/micropython/upstream/ports/esp32/main.c:151-162`) is:

1. `pyexec_frozen_module("_boot.py", false)` — runs the **frozen `_boot.py`** (underscore). The
   stock one mounts the flash FS at `/` (`import vfs; from flashbdev import bdev; vfs.mount(...)`),
   falling back to `inisetup.setup()` on first boot (which writes a comments-only flash `/boot.py`).
2. `pyexec_file_if_exists("boot.py")` — runs the flash **VFS** `/boot.py` if present.
3. `pyexec_file_if_exists("main.py")` — runs the flash **VFS** `/main.py` if present.

So the **only frozen module auto-run at boot is `_boot.py`.** A `module("boot.py", …)` or a frozen
`main.py` in the manifest is **not** in the exec chain — `boot.py`/`main.py` are read from the VFS,
not `.frozen`. Freezing them only makes them importable, never auto-run.

**Consequence — a frozen firmware has no app launcher of its own.** The app modules are frozen and
importable, but nothing calls `Controller.start()` until a **flash `/main.py`** does. The deploy
tools write that launcher (`tools/deploy_app.py --mode run`, `tools/set_p4_boot_app.py`); a freshly
flashed image (whose VFS holds only the comments-only `/boot.py` from `inisetup`) therefore **boots
to the REPL, not the app.** A self-launching standalone image must carry a `/main.py` in the flashed
VFS partition (or override the frozen `_boot.py` to launch), not merely freeze the app.

This is also why the overlay dev lane's `sys.path.insert(0, "/overlay")` belongs in that **launcher
`/main.py`** (which always runs before the app import), not in a frozen `boot.py`.

## To enable a flash-override dev lane

Prepend a writable dir ahead of `.frozen` — the entry `/main.py` doing
`import sys; sys.path.insert(0, "/overlay")` (see the boot-exec-order section above for why the
launcher, not a frozen `boot.py`, is the right vehicle) — then write the whole `seedsigner` package
to `/overlay/seedsigner`. Cost: one failed front-of-path `stat` per unresolved top-level import when
the overlay is empty; a full (slow, ~60-70 ms/`stat`) VFS import for whatever package you do overlay
— which is exactly why the app is frozen in the first place, so overlay only the package under
active edit. The bench harnesses already use the root-prepend idiom (`sys.path.insert(0, '/')`).

See the "Frozen app build & versioning" section of `README-dev.md` for how this is wired into the
build + deploy tooling.
