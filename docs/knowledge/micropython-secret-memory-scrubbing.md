# Scrubbing secret buffers on MicroPython: freed ≠ wiped, and how to actually erase

Verified against the pinned MicroPython v1.27.0 source this session while wiring the
image-entropy seed path. These are the VM mechanics behind the "return `bytearray`, scrub in
place, then `gc.collect()`" pattern. The *design decision* (keep hashing in readable Python; the
bindings hand back mutable buffers the app overwrites) lives in the app repo's untracked
`docs/_integration/image-entropy-memory-clearing-improvements-todo.md`; this doc is the durable
"why it has to be this way."

## 1. `= None` frees on CPython, does nothing immediate on MicroPython

- **CPython** refcounts: dropping the last reference deallocates the buffer immediately.
- **MicroPython** is pure mark-and-sweep with **no refcounting** (`py/gc.c`). `x = None` only
  marks the object unreachable; nothing is reclaimed until a sweep runs. Hence the explicit
  `gc.collect()` after dropping secret references — it turns "eventually" into "now."

## 2. Neither VM *zeroes* freed memory — freeing is not erasing

`py/gc.c` defines `CLEAR_ON_SWEEP (0)`, so swept blocks are **not** overwritten. The plaintext
sits in freed-but-unscrubbed memory until some later allocation happens to reuse those blocks. So
`= None` + `gc.collect()` bounds *lifetime as a live object* but leaves the bytes readable. To
actually erase, you must overwrite the contents **before** dropping the reference. This is why
immutable `bytes` is the wrong return type for secret data — it can only be de-referenced, never
scrubbed.

## 3. Return `bytearray`, not `bytes`, from bindings that hand over secrets

- `mp_obj_new_bytearray(len, ptr)` **copies** out of the native buffer (peak = 2× the buffer size
  while native latch + Python object coexist — matters when the buffer is a multi-MB camera
  frame). The copy is unavoidable for a value-semantics object; the point of `bytearray` over
  `bytes` is purely that the copy is then *scrubbable*.
- `bytearray == bytes` compares by **buffer contents**, not type (`py/objarray.c`
  `array_binary_op` → `MP_BINARY_OP_EQUAL` compares the raw buffers). So existing sanity checks
  like `buf == bytes([buf[0]]) * len(buf)` keep working unchanged after switching the return type.

## 4. A correct in-place scrub primitive needs care (two traps)

Exposed as `camera_entropy.secure_zero(buf)`:

- **Must be in-place.** `buf[:] = b"\x00" * len(buf)` is allowed to *reallocate*, which frees the
  original plaintext buffer un-scrubbed — the exact opposite of the goal. Writing through the
  buffer protocol (`mp_get_buffer_raise(buf, &info, MP_BUFFER_RW)` then memset `info.buf`) touches
  the caller's own memory.
- **Must survive dead-store elimination.** A plain `memset` over memory never read again is a dead
  store the compiler may drop. Reach `memset` through a **`volatile` function pointer**
  (`static void *(*const volatile secure_memset)(void*,int,size_t) = memset;`) — the compiler must
  load and call through it, keeping real-memset speed for MB-sized frames.
- Take the buffer as `MP_BUFFER_RW` so passing immutable `bytes` **raises** rather than silently
  no-opping — a scrub that couldn't land should fail loudly. (This also usefully catches tests
  that hand-build `bytes` where production returns `bytearray`.)

## 5. `hashlib.sha256` is streaming — fold big inputs, don't concatenate

`sha256(acc + big)` builds a full-size temporary copy of `big` just to hash it. `sha256(acc)` then
`.update(big)` is **byte-identical** (`extmod/modhashlib.c` is a thin mbedtls wrapper; verified
equal to the concatenation form) and skips the copy. For the entropy final image this removes a
multi-MB throwaway allocation. Note `.update()` returns `None`, and after `.digest()` the object is
finalized (`self->final = true` → further update/digest raise) — so no `.copy()`, one digest.

## 6. GC can return a big allocation's memory to the system — but only best-effort

MicroPython's heap is a split heap that grows on demand (`MICROPY_GC_SPLIT_HEAP` +
`_AUTO`). A large allocation (a multi-MB camera still) forces a new area sized
`MAX(total_heap, needed)`. `gc_sweep_free_blocks` **does** free an entire empty area back via
`MP_PLAT_FREE_HEAP` (`py/gc.c` ~line 660) — but **only if** `last_used_block == 0`, i.e. the whole
area is empty, and it's not the initial area. Consequences:

- The collector is **non-moving** — it never compacts, so it cannot close gaps. A single small
  long-lived object landing in that area **pins the whole multi-MB region**.
- Therefore: keep the secret allocation as *small as the job allows* (tighter area = fewer
  stragglers to pin it), and `gc.collect()` after dropping it so the empty-area release actually
  fires. Reclamation of the *value* is guaranteed; return of the *area* to the system is not.

## Cross-refs

- Native side already scrubs its own buffers: `cam_pipeline_entropy` calls
  `mbedtls_platform_zeroize` on the latch/chain at teardown. The app-side scrub covers the *copies*
  that cross into the VM.
- App decision doc (untracked): `seedsigner/docs/_integration/image-entropy-memory-clearing-improvements-todo.md`.
- Memory: `feedback_no_hardware_randomness` (why entropy sources are camera-only, never HW RNG).
