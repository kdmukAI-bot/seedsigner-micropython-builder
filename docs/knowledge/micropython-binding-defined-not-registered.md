# A MicroPython binding function can be fully defined yet unreachable — compiles clean, `AttributeError` at runtime

## Symptom

`camera_entropy.secure_zero(...)` raised `AttributeError: 'module' object has no attribute
'secure_zero'` on-device — even though the function was fully implemented in
`bindings/modcamera_entropy.c`, documented in the module header comment, and the firmware built
with **no error or warning**.

## Root cause

A MicroPython C function becomes callable from Python only when **both** of these exist:

1. The function object — `static mp_obj_t mp_..._secure_zero(...)` plus
   `MP_DEFINE_CONST_FUN_OBJ_1(camera_entropy_secure_zero_obj, ...)`.
2. **An entry in the module's globals table** —
   `{ MP_ROM_QSTR(MP_QSTR_secure_zero), MP_ROM_PTR(&camera_entropy_secure_zero_obj) }` inside
   `..._module_globals_table[]`.

Step 1 without step 2 produces a **valid, unused `static`**. The compiler is completely happy —
an unused static function object is not even a warning-level event — so there is no build signal.
The QSTR scanner also does its job: `MP_QSTR_secure_zero` only becomes a real QSTR if the
`MP_ROM_QSTR(MP_QSTR_secure_zero)` token appears somewhere, and if you forgot the table entry, it
never appears. Everything is internally consistent; the function is simply not wired to the name.

The failure therefore surfaces **only at the call site, at runtime**, which for a
seed-derivation scrub is deep in a security-critical path (after the mnemonic is generated, before
`set_pending_seed`) — exactly where you least want a first-run crash.

## Why it's easy to miss

- `py_compile` on the *Python* side passes — the attribute error is a runtime lookup, invisible to
  static analysis.
- The firmware *build* passes — see above.
- It only reproduces on-device, or in a host test that actually calls the method through the real
  module.

## The check (run on every binding edit)

Every `MP_DEFINE_CONST_FUN_OBJ*`-declared `_obj` must appear in exactly one `MP_ROM_PTR(&…)` table
entry. One-liner audit per file:

```sh
for o in $(grep -oP 'MP_DEFINE_CONST_FUN_OBJ\w*\(\K\w+_obj' bindings/modX.c | sort -u); do
    grep -q "MP_ROM_PTR(&$o)" bindings/modX.c || echo "MISSING: $o"
done
```

## Broader lesson

"Compiles clean" is not verification for bindings. The two independent gates that can silently
diverge are (a) does the C build succeed and (b) is the symbol reachable from Python. Only running
the code — pytest against the real/stubbed module, or a firmware flash + on-device exercise —
closes the gap. This session shipped-then-caught this exact defect, alongside three broken tests
that also passed `py_compile`.

Related: `docs/knowledge/locale-picker-endonym-provider-micropython.md` (QSTR-scan include-path
constraints — a *different* way a binding symbol fails to materialize).
