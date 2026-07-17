# `button_list_screen` "invalid JSON" on longer configs — vstr wasn't null-terminated

## Symptom
`seedsigner_lvgl_screens.button_list_screen(cfg)` (and every screen routed
through `run_cfg_screen` in `bindings/modseedsigner_bindings.c`, plus the locale
picker) intermittently raises `ValueError: invalid JSON <garbage>` — the trailing
bytes are non-printable junk (e.g. `synd\x05\x03O\x17`). It depends on the
**total serialized size** of the config, not its content: a menu of 5 long
button labels renders fine; adding a 6th long label makes the identical code path
throw. Short labels at the same button count render fine.

## Root cause
`run_cfg_screen` builds the config JSON into a MicroPython `vstr`:

```c
vstr_t json;
vstr_init(&json, 256);
...
vstr_add_json_from_obj(&json, args[0]);
const char *err = run_screen(fn, (void *)json.buf);   // <-- BUG
```

`vstr_add_*` **do not keep a trailing `\0`**, and `vstr_init(&json, 256)` does
not zero its backing allocation. `run_screen` (and the C++ screen code behind it)
treats the pointer as a NUL-terminated C string. As long as the serialized JSON
stays under the initial 256-byte alloc, the bytes past `json.len` happen to be
whatever the allocator left there — often a `0` early enough that the parser
stops cleanly, so it "works." Once the JSON grows to ~256 B, the parser runs off
the end into uninitialized heap and nlohmann reports a syntax error at the first
garbage byte. It is a latent, size-dependent bug that had been masked by luck.

The garbled error *text* is a second, smaller bug on the same lines: `err` can
point into `json.buf`, which `vstr_clear(&json)` frees before
`mp_raise_msg_varg` reads it — a use-after-free on the message only (cosmetic;
the parse error itself is real). The `clear`-before-`raise` order is deliberate
(avoids leaking the vstr on the `mp_raise` longjmp), so it was left as-is.

## Fix
Pass the null-terminated form. MicroPython provides exactly this:

```c
const char *err = run_screen(fn, (void *)vstr_null_terminated_str(&json));
```

Applied at both `run_screen(..., json.buf)` sites in
`bindings/modseedsigner_bindings.c` (`run_cfg_screen` and the locale picker).
`vstr_null_terminated_str` ensures capacity for `len+1`, writes the terminator,
and returns the (possibly moved) buffer, which is then freed by the existing
`vstr_clear`.

## Why it matters beyond instrumentation
This is a **production-app** bug: every `button_list_screen` / `main_menu_screen`
goes through `run_cfg_screen`. Any real screen whose localized title + labels
serialize past ~256 B (long translations, many buttons) can hit it
nondeterministically. The instrumentation build surfaced it because a 6-item
`_tap_prompt` menu with long labels crossed the threshold. The fix belongs in the
mainline `bindings/modseedsigner_bindings.c`, not just the instrumentation tree.

## How it was found (2026-07-17)
Adding a 6th "RUN B" entry to the instrumentation sequencer's boot menu made the
menu crash at boot. On-device REPL A/B (`button_list_screen` with 2 / 5-long /
6-long / 6-short button lists) isolated it to total JSON length, not count or
content — 6 short labels rendered, 6 long labels did not. The serializer
(`vstr_add_json_from_obj`) was verified correct (proper escaping, dynamic
growth), which pointed at the consumer treating a non-terminated buffer as a C
string.
