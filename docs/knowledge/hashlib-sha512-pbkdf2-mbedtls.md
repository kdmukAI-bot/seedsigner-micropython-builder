# Adding SHA-512 + PBKDF2 to MicroPython's hashlib (mbedtls-backed), patch-free

**Status:** built + P4-validated 2026-07-05. Unblocks BIP32/BIP39 (and thus PSBT
processing + signing) on-device, which were failing on missing hashes. Files:
`deps/esp-hashlib-ext/` (component), `bindings/modhashlibext.c` (binding),
`deps/third-party/hashlib.py` (frozen shim), P4 board `manifest.py` (freeze).

## The gap

MicroPython v1.27.0's `hashlib` (`extmod/modhashlib.c`) ships only **SHA-256** (and
SHA-1/MD5 gated on `MICROPY_SSL`, which is off here — network stripped). There is **no
`MICROPY_PY_HASHLIB_SHA512` symbol at all** — SHA-512 is simply not implemented — and
**no `pbkdf2_hmac`** (that's a CPython-only hashlib function). The bundled
`lib/crypto-algorithms/` also has only sha256. So on-device:

- `bip32.HDKey.from_seed` / `.derive` → `hmac.new(..., 'sha512')` → `AttributeError:
  'module' object has no attribute 'sha512'` (BIP32 CKD is HMAC-SHA512).
- `bip39.mnemonic_to_seed` → `hashlib.pbkdf2_hmac('sha512', ...)` → `AttributeError`.

Both are hard requirements (SeedSigner `seed.py` calls them directly), independent of the
native secp256k1 work — they blocked any on-device derivation/signing/PSBT-parse.

## Where the implementations come from: the already-linked mbedtls

The ESP-IDF `mbedtls` component is already compiled into the firmware (cUR uses it for
SHA-256). It provides, both enabled by default in IDF's `mbedtls_config.h`:
`mbedtls_sha512` (`MBEDTLS_SHA512_C`) and `mbedtls_pkcs5_pbkdf2_hmac_ext`
(`MBEDTLS_PKCS5_C`). So no new dependency — just surface them to Python.

## Two non-obvious constraints

### 1. `hashlib` is an *extensible* module → extend it with a frozen `hashlib.py` (no C patch)

`modhashlib.c` ends with `MP_REGISTER_EXTENSIBLE_MODULE(MP_QSTR_hashlib, ...)`. Per
`py/builtinimport.c`, importing an extensible module tries the **filesystem/frozen first**,
then the built-in. So a frozen `hashlib.py` *shadows* the built-in, and the built-in stays
reachable via its u-prefix alias `uhashlib`. The shim is just:

```python
from uhashlib import *                       # built-in SHA-256 (etc.)
from _hashlib_ext import sha512, pbkdf2_hmac  # native mbedtls-backed additions
```

This means **zero changes to `modhashlib.c`** (and thus to the shared 0001 patch) — the
whole feature is a native helper module + a frozen shim + build wiring.

### 2. HMAC-SHA512 needs `block_size = 128` on the hash object, or it computes WRONG

MicroPython's built-in hash types expose only `update`/`digest` — no `block_size`. The
frozen `hmac` reads `hash.block_size` and **defaults to 64** when absent
(`micropython-lib .../hmac.py`). SHA-256's block size *is* 64, so it works by luck. SHA-512's
block size is **128**; if the type doesn't advertise it, HMAC pads the key to 64 and every
HMAC-SHA512 — i.e. every BIP32 CKD step — produces wrong output (wrong keys, silently).
So the `sha512` type's `locals_dict` MUST include `{block_size: 128}` (and `digest_size:
64`). It also needs `copy()` (mbedtls_sha512_clone) — `hmac._current()` clones the hash.
Validated with RFC-4231 HMAC-SHA512 test case 1 on-device.

## Why a component, not mbedtls-in-the-usermod

First attempt linked `__idf_mbedtls` into the usermod so the binding could `#include
"mbedtls/sha512.h"`. That **fails**: MicroPython's usermod collects linked libs'
`INTERFACE_INCLUDE_DIRECTORIES` for the QSTR scan and does **not** evaluate CMake generator
expressions, so mbedtls's `$<BUILD_INTERFACE:...>` include leaks through literally →
`CMake Error ... ports/esp32/$<BUILD_INTERFACE:.../mbedtls/include/>`. It also made
MicroPython treat `__idf_mbedtls` as a user C module.

Fix = the same plain-C-lib split cUR/esp-secp256k1 use: a tiny IDF **component**
(`deps/esp-hashlib-ext/`, `REQUIRES mbedtls`) exposing a plain-C API
(`hlx_sha512_*`, `hlx_pbkdf2_sha512`); the usermod binding includes only that plain header
and links `__idf_esp-hashlib-ext`. mbedtls's headers/config/genexes stay inside the
component. The opaque SHA-512 context is sized via `hlx_sha512_ctx_size()` and stored inline
in the GC hash object (mbedtls's context owns no heap, so no finaliser needed).

## Scope note

`pbkdf2_hmac` here supports only `'sha512'` (all SeedSigner/embit callers use it). Custom
digests would need the md_type mapped through. This is the ESP32/mbedtls path; a CPython
(Pi Zero) build already has both from stock hashlib.
