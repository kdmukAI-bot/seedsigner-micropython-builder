# Native libsecp256k1 on ESP32 MicroPython requires static ECMULT precomputation

**Status:** validated on ESP32-P4 (Waveshare LCD 4.3), 2026-07-04, during the Phase-0
spike of the native `secp256k1` C module (`deps/esp-secp256k1/`).

## Symptom

The first call into the native module that creates the signing context
(`secp256k1_context_create`, triggered lazily by `ec_pubkey_create`) crashes the board
with:

```
Guru Meditation Error: Core 1 panic'ed (Stack protection fault).
Detected in task "mp_task" at 0x480ff6f8
Stack bounds: 0x4ff349c4 - 0x4ff389c0        <- ~16 KB
```

The register dump is full of generator-point fragments (`0x02f81798`, `0x0206295c`), i.e.
the EC math was mid-flight when the stack blew.

## Root cause

`secp256k1-zkp @ d9560e0` (embit 0.8.0's pinned commit) is the *old-style* secp256k1 that,
without `USE_ECMULT_STATIC_PRECOMPUTATION`, builds the signing generator table **at
runtime** in `secp256k1_ecmult_gen_context_build()` (`src/ecmult_gen_impl.h`). That
function declares the working tables **on the stack**:

```c
secp256k1_ge  prec[ECMULT_GEN_PREC_N * ECMULT_GEN_PREC_G];   // ~88 KB
secp256k1_gej precj[ECMULT_GEN_PREC_N * ECMULT_GEN_PREC_G];  // ~128 KB
```

With embit's embedded config (`ECMULT_GEN_PREC_BITS = 4`): `N = 256/4 = 64`,
`G = 1<<4 = 16` → `N*G = 1024` entries each, **~200 KB of stack** in one call.

MicroPython's `mp_task` stack is `MICROPY_TASK_STACK_SIZE = 16 * 1024` (16 KB, set in
`ports/esp32/mpconfigport.h`). 200 KB ≫ 16 KB, so it faults instantly. No practical stack
bump fixes this (you would need a >200 KB task stack forever).

## Fix

Enable **`USE_ECMULT_STATIC_PRECOMPUTATION`** in the component's
`config/libsecp256k1-config.h`. This:

- moves the 1024-entry generator table to a `const` array in flash `.rodata`
  (`generated/ecmult_static_context.h`), so **`context_create` no longer builds it** — no
  200 KB stack, no ~64 KB heap table, near-instant context creation;
- leaves the small *verify-side* ecmult table (`ECMULT_WINDOW_SIZE = 4` → 4 entries) to be
  built at runtime, which costs **< 1 KB** of stack — well within 16 KB.

Measured result after the fix: `secp256k1_context_create` consumes **708 bytes** of heap
(vs the plan's feared ~64 KB), and `ec_pubkey_create(1)` returns the generator point G
byte-identically to embit's pure-Python fallback.

### Generating the static header

`gen_context.c` (vendored in the zkp tree) is the codegen tool. The table depends only on
the fixed `ECMULT_GEN_PREC_BITS` / field backend, so it is generated **once on the host**
and vendored — no build-time host-codegen step:

```sh
cd deps/esp-secp256k1/secp256k1-zkp
cc -O1 -I. -Iinclude -I../config -DHAVE_CONFIG_H -o /tmp/gen_context src/gen_context.c
./  # run with CWD = repo root; writes src/ecmult_static_context.h (1024 SC(...) entries)
# then move it OUT of the submodule so the submodule stays pristine:
mv src/ecmult_static_context.h ../generated/ecmult_static_context.h
```

The generated header does `#include "src/group.h"`, so the `secp256k1-zkp` root must be on
the component's include path (it is, in `PRIV_INCLUDE_DIRS`). It also self-checks
`ECMULT_GEN_PREC_N/G` against the config — regenerate if `ECMULT_GEN_PREC_BITS` ever changes.

Compiling `gen_context` on the host with the **same** field backend (`USE_FIELD_10X26` /
`USE_SCALAR_8X32`) as the target keeps `SECP256K1_GE_STORAGE_CONST` packing identical; the
table *values* are curve math and architecture-independent. The `privkey=1 → G` device test
confirms host-generated tables are correct on-target.

## Why embit's own config leaves it off

embit's `libsecp256k1-config.h` `#undef`s `USE_ECMULT_STATIC_PRECOMPUTATION` because embit
runs on desktop CPython, where the 200 KB build happens on a multi-MB thread stack and the
runtime-build flexibility (any PREC/WINDOW without regenerating) is worth more than the
one-time cost. On a 16 KB-stacked microcontroller task the trade-off inverts: static is
mandatory.

## Note: modern secp256k1 does not have this problem

Post-2021 `bitcoin-core/secp256k1` ships checked-in precomputed tables
(`precomputed_ecmult*.c`) and dropped the runtime-build path entirely. This constraint is
specific to the older `d9560e0` era we pin for embit-ABI parity. If we ever move to a modern
zkp/upstream commit, the static-context generation step goes away.
