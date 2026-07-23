"""Single source of truth for the frozen-app launcher written to the device as /main.py.

MicroPython auto-runs `/main.py` at boot (after the frozen `_boot.py` mounts the flash
filesystem at `/` and the flash `boot.py` runs). A frozen firmware freezes the whole app
but has NO launcher of its own -- nothing calls `Controller.start()` until this `/main.py`
does -- so a freshly flashed image boots to the REPL until one is written (by
tools/deploy_app.py --mode run, tools/set_p4_boot_app.py, or the baked dist image).

The launcher prepends `/overlay` to `sys.path` so a dev-overlay package
(tools/deploy_app.py --mode overlay) can shadow the frozen `seedsigner` for fast iteration
without a firmware rebuild. When `/overlay` is empty/absent the only cost is one failed
front-of-path `stat` per unresolved top-level import, and the frozen app runs unchanged.
Package resolution is atomic per top-level name, so an overlay must shadow the WHOLE
`seedsigner` package; `embit`/`urtypes` stay frozen. See
docs/knowledge/micropython-frozen-vs-vfs-override.md for the sys.path / boot-exec mechanism.

Kept in ONE place so the three launcher writers -- deploy_app.py, set_p4_boot_app.py, and
the dist bake (tools/build_launcher_fs.py) -- can't drift out of sync.

The [boot-ms] prints are launch-timing milestones: time.ticks_ms() counts from power-on and
splits the Python phase into VM-boot, the seedsigner.controller import chain, and Controller
construction. On any launch error the app prints the traceback and drops to the REPL so the
board stays recoverable over serial (no boot-loop).
"""

MAIN_PY = """\
# SeedSigner app launcher. Auto-runs at boot after _boot.py / boot.py.
# Prepend /overlay so a dev-overlay package (tools/deploy_app.py --mode overlay)
# can shadow the frozen seedsigner; when /overlay is empty this costs one failed
# stat per top-level import and the frozen app runs unchanged.
import sys
sys.path.insert(0, '/overlay')
import time
print('[boot-ms] main.py start:', time.ticks_ms())
try:
    from seedsigner.controller import Controller
    print('[boot-ms] controller import done:', time.ticks_ms())
    _c = Controller.get_instance()
    print('[boot-ms] controller instance ready, calling start():', time.ticks_ms())
    _c.start()
except Exception as e:
    sys.print_exception(e)
    print('[main] Controller exited via exception; dropping to REPL.')
"""
