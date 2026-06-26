# P4 deploy fails with `NameError: name '_w' isn't defined` — a stale `/main.py` is auto-running

## Symptom
`tools/deploy_app.py` (any mode) aborts during the file push with a device-side
traceback:

```
RuntimeError: device error:
Traceback (most recent call last):
  File "<stdin>", line 1, in <module>
NameError: name '_w' isn't defined
```

`_w` / `_wa` are the on-device write helpers `deploy_app.py` injects via
`raw_exec(ser, DEVICE_HELPERS)` before streaming files. The error says they were
never defined — but the real cause is upstream of that.

## Root cause
A **stale `/main.py` on the P4's internal-flash VFS is auto-running the previous
app at boot**, and that occupies the REPL so the helper-definition exec doesn't
land cleanly.

Two facts combine to make this non-obvious:

1. **A firmware reflash does NOT wipe the VFS.** `esptool write_flash` of
   `micropython.bin` only rewrites the app partition (`0x10000`). The MicroPython
   filesystem lives in a separate data partition that survives the flash, so
   `/main.py`, `/boot.py`, and `/lib/seedsigner` from prior testing remain. A
   fresh-feeling firmware flash still boots straight into the old app.
2. **`deploy_app.py` poll-opens the REPL with `do_reset=False`** (see `main()` →
   `hard_reset_and_wait(port, do_reset=False)`). Opening the USB-serial port
   soft-resets the board (DTR), which re-runs `main.py`; the tool then relies on
   `Ctrl-C` to break into a clean prompt. When `main.py` is a long-running app
   loop, that break-in is racy — the first `raw_exec(DEVICE_HELPERS)` can be
   swallowed/mangled, leaving `_w` undefined, and the push then fails.

(The generated `MAIN_PY` wraps `Controller.start()` in `except Exception`, not
`BaseException`, so `KeyboardInterrupt` *does* propagate to the REPL — the problem
is timing/contention during the no-reset poll-open, not that Ctrl-C is caught.)

## Diagnosis
Hard-reset into a bare REPL and list `/`:

```python
from sd_format_push import hard_reset_and_wait, raw_exec  # tools/
ser = hard_reset_and_wait("/dev/ttyACM0", do_reset=True)
print(raw_exec(ser, "import os; print(os.listdir('/'))"))
```

If you see `main.py` in the listing, that's it.

## Fix
Remove the stale entry point, then redeploy clean:

```python
raw_exec(ser, "import os; os.remove('/main.py')")   # stop the auto-run
```
```
python3 tools/deploy_app.py --mode import-smoke --clean   # wipe /lib + re-push
python3 tools/deploy_app.py --mode run
```

`--clean` removes `/lib/seedsigner` and `/lib/embit` first, so you also avoid
stale-file mismatches from an older app tree.

## Takeaway
When `deploy_app.py` behaves strangely on a board that previously ran the app,
suspect a persisted `/main.py` before suspecting the deploy tooling. The VFS
outlives firmware flashes; a clean deploy wants a bare REPL (no auto-run app)
to start from. A future hardening option: have `deploy_app.py` hard-reset and
remove `/main.py` (or interrupt before `main.py` runs) at the start of every
deploy, rather than poll-opening into whatever is running.
