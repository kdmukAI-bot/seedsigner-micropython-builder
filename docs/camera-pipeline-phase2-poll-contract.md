# Camera Pipeline — Phase 2 Poll Contract (scan_coordinator redesign)

**Status:** DESIGN / for review — not yet implemented. Working-tree only (not committed).
**Date:** 2026-06-30
**Supersedes the in-task callback model** of `scan_coordinator` for the firmware path.

This note records the contract converged on in design discussion for wiring the
camera QR pipeline into MicroPython firmware so the Python `DecodeQR` flow drives
the LVGL `camera_preview_overlay`. It reshapes `scan_coordinator` (board_common)
from an in-task injected-classifier into a **decoupled, poll-based glue layer**.

---

## 1. Why poll, not synchronous in-task

The current `scan_coordinator` calls an injected `classify()` **synchronously on the
decode task** ("must return quickly"). That was the simplest thing for the C sample
apps, but it is wrong for production:

Three stages run at **independent rates** and must not gate each other:
- **camera frame rate** — sensor/pipeline (~13–15 fps idle)
- **decode rate** — k_quirc CPU cost (~3.5–5 decodes/s, the spike target)
- **UI refresh** — overlay wants updating "a few times per second" (its own header)

Coupling them onto one task means the slowest stalls the fastest. The dangerous
one is `classify`: in production it is `DecodeQR.add_data` — stateful fountain/BBQr
assembly with **spiky, variable latency** (the last part triggers full reassembly +
checksum). Running that on the decode task blocks intake of the next camera frame
during animated-QR scanning — exactly when frames must not drop. Also: a Python
classifier runs on the MicroPython task and cannot be called from the decode task
without a blocking cross-task round-trip per frame.

The PoC already leaks this coupling: `overlay_present` takes the LVGL lock with a
timeout and **drops on contention**. That drop is a patch over a coupling that
shouldn't exist; with a buffer between stages it becomes a natural, lossless gap.

**Conclusion:** the decode task does ONE job — pull frames, run the engine, enqueue
outcomes. Classification and presentation run on the consumer's own clock.

In-task synchronous is only ever right for a **trivial classifier with no separate
consumer task** (a self-contained C demo) — the sample-app shape, not production.

---

## 2. Layering — where the glue lives

- **`scan_coordinator` (board_common):** the decoupled glue. Platform-neutral: it
  transports decoded outcomes out (poll) and takes (status, percent) back (report),
  dedups raw payloads, and dispatches an **injected** `present`/`on_complete`. It has
  **no MicroPython or LVGL dependency** — the LVGL lock and any VM specifics live in
  whoever supplies `present`.
- **Builder binding (`bindings/` + a small `camera_scanner` C module under
  `ports/esp32`, mirroring `display_manager`):** the thinnest possible MicroPython
  skin over `poll`/`report`, plus a `present` callback that drives
  `camera_preview_overlay` under the LVGL port lock. Builder-specific because the
  binding is MicroPython-specific.

This keeps one implementation of the scan contract (no C-reference-plus-Python-copy
duplication), correct altitude (above the engine, below the UI), and clean layering
(builder → board_common, never the reverse).

---

## 3. Threading model

```
decode task:    on_frame(outcome, payload) ─► transport-dedup ─► enqueue event to ring
                                              (ONLY thing on this task)

consumer task:  scan_coordinator_poll()    ─► next { kind, payload? }
                  NEW → DecodeQR.add_data() → (domain status, percent)   [classify here]
                scan_coordinator_report(status, percent)
                                              ─► domain dedup ─► present()   [consumer task]
                                              ─► COMPLETE → on_complete()
```

- decode task **only enqueues**; never classifies, never presents, never takes the
  LVGL lock.
- `present` / `on_complete` fire on the **consumer's task**, so they may safely touch
  Python and LVGL.

---

## 4. Event kinds (what `poll()` returns)

```
poll() -> { NEW(payload) | REPEAT | MISS | NOTHING }

  NEW     → DecodeQR.add_data(payload) → PART_COMPLETE / PART_EXISTING / COMPLETE
            → overlay: green / gray / done
  REPEAT  → same QR still held in frame. No payload, no classify.
            Consumer ignores OR sets gray dot.
  MISS    → located but not decoded. Surfaced as a coordinator-maintained
            `consecutive_misses` counter (any non-MISS frame resets it), NOT discrete events
            (see §5/§7a). Consumer ignores a transient MISS for the dot for now, but
            the counter feeds a sustained-MISS ("found but unreadable") warning later.
  NOTHING → nothing in frame → dot hidden. Dedup'd to transitions, not every idle frame.
```

---

## 5. Two-layer deduplication

Two distinct "duplicates" at two layers — split deliberately:

**Transport dedup — coordinator, decode task, at enqueue (cheap, raw bytes):**
"Is this decoded payload byte-identical to the one I *just* forwarded?" A held-static
QR is the same `DECODED` bytes every frame. Detect via `memcmp` against a retained
last-payload buffer (or a retained hash). On a match, enqueue a tiny **REPEAT marker
instead of re-copying the payload**.
- **Consecutive-only**, NOT set-membership. `A A A B B A A` → `NEW(A),REPEAT,REPEAT,
  NEW(B),REPEAT,NEW(A),REPEAT`. The second `A` ships in full because it differs from
  the last-forwarded (`B`); the consumer's `DecodeQR` then decides it's
  `PART_EXISTING`. Collapsing all-time repeats would need decoder state — wrong layer.
- State cost: one last-payload buffer. `memcmp` of ~1 KB on the decode task is
  microseconds vs the millisecond-scale decode — negligible.

**Domain dedup — consumer / DecodeQR (stateful):**
"Is this a part I've already assembled?" → `PART_EXISTING` (e.g. animated QR looped
back). Needs the decoder's accumulated state; can only live in the consumer.

**Double win** for the common case (static QR held steady, many frames): payload
bytes cross the ring **once**, then tiny markers — (1) no repeated ring copy (keeps
the ring small; matters for ~1 KB BBQr/PSBT parts), and (2) the consumer skips
`DecodeQR.add_data` entirely on REPEAT.

The coordinator only *informs* ("same one again"); the consumer always decides the
screen reaction.

**Dedup principle (governs ALL collapsing in the coordinator):** the coordinator may
only collapse a repeated outcome when its repetition carries **no information**.
- **NEW→REPEAT** (identical bytes): repetition is null → collapse to a marker. ✓
- **NOTHING**: repeated "no code in frame" is null → dedup to transitions. ✓
- **MISS**: repetition IS the signal — sustained MISS = "we keep locating a code here
  but can't decode it" (damaged QR, or a hand-transcribed SeedQR with too many
  transcription errors; valid finder patterns → MISS, not NOTHING). Collapsing it away
  forecloses a real, SeedSigner-specific UX affordance. → MISS is surfaced as a
  **`consecutive_misses` counter** (§7a): increments on MISS, **reset by ANY non-MISS
  outcome** — a decode (NEW/REPEAT) proves readability, and NOTHING ends the attempt so
  a reposition / new code must re-accumulate. The reset lives in the coordinator because
  it sees every frame, including the REPEATs a held already-decoded QR streams that the
  consumer's coarse poll would miss. A scanner truly vacillating NONE↔MISS keeps the run
  low — correctly: that's the edge of detection (focus/damage) where silence is the
  honest answer. The consumer just thresholds the value.

A MISS does **not** reset the transport "last forwarded payload" identity: a momentary
MISS between successful reads of the same static QR leaves the data unchanged, so
re-acquisition is correctly a REPEAT (no new info), not a re-shipped NEW.

---

## 6. Overlay mapping (current decision)

| Outcome | Overlay | Rationale |
|---|---|---|
| NEW → PART_COMPLETE | green dot, bar advances | new info added |
| NEW → PART_EXISTING | gray dot | read OK, no new info (domain repeat) |
| REPEAT | gray dot | read OK, no new info (transport repeat) |
| MISS | dot unchanged/hidden **for now** (but stream NOT deduped, §5) | transient miss ignored; sustained MISS reserved for a future "found but unreadable" warning |
| NOTHING | dot hidden | nothing in frame |
| COMPLETE | bar full + green, terminal | assembled |

Updates are **absolute snapshots** (`set_progress(percent, dot)`), not increments —
so dropped/coalesced intermediate updates self-heal; the screen converges to truth
with bounded lag, never goes backwards, and strictly lags (never leads) the decoder.

---

## 7. Open items / later TODOs

- **MISS vs REPEAT as distinct user signals.** The gray dot historically meant "this
  animated frame added no new information" (a successful-but-redundant read). MISS is
  semantically different: the decoder *saw and located* a QR but *could not read it* —
  actionable feedback ("hold steadier / adjust focus / lighting"), not a benign
  redundant read. Conflating them would hide a real failure as a benign repeat.
  Decision **for now:** consumer ignores a transient MISS for the dot; REPEAT → gray.
  But MISS is **not deduped in the coordinator** (see §5) so the stream survives for a
  future warning.
  - **MEASURED on device (2026-06-30, scan_coord_test on P4 LCD 4.3):** MISS is the
    DOMINANT outcome during normal animated scanning — ~74 MISS events against ~6
    successful NEW decodes in one session, dozens of MISS between each decode. So a
    raw climbing *total* miss count is the *normal, healthy* state, NOT a damage
    signal. The naive "sustained MISS → unreadable" heuristic would fire on every scan.
  - **Resolved by `consecutive_misses` (§5/§7a):** a run of MISS frames reset by ANY
    non-MISS outcome (decode NEW/REPEAT, or NONE), so it climbs only across frames where
    a code is located-but-unread with nothing else interrupting — i.e. miss-WITHOUT-
    progress *directly*. MISS interleaved with periodic decodes keeps it near zero; MISS
    accumulating while nothing else happens = "found a code, can't read it" → likely
    damaged QR, or a hand-transcribed SeedQR with too many mistakes (re-check
    transcription / clean or reprint / improve lighting). The reset lives in the
    coordinator because a held already-decoded QR streams REPEATs (no NEWs) the
    consumer's coarse poll can't all see. NONE resets too so a look-away / switch to a
    second code re-accumulates fresh; a true NONE↔MISS vacillation (edge of detection)
    correctly stays below threshold — silence is the honest answer there. High-value,
    still-future signal; the consumer owns the threshold.
  - **THRESHOLD MEASURED (2026-06-30, P4 LCD 4.3):** runs reached **32** while scanning a
    dense 3-of-5 multisig descriptor *deliberately too far / out of focus* (below the
    px/module floor); moving closer fixed it and it decoded. So that run was NOT a false
    positive — it was a genuine **user-correctable** situation (too far / poor focus /
    too dense for the distance / lighting), which is the *common* trigger; "damaged QR /
    bad transcription" is the rare tail. The warning message should therefore be a nudge
    ("found a code but can't read it — move closer / steady up / improve lighting",
    damage as fallback), pairing with the overlay framing guide.
  - **Threshold is "nudge latency," not a false-alarm floor.** The only TRUE false
    positive is firing while a scan is *actively progressing*, and reset-on-decode
    already kills that (every decode zeroes the run); long runs occur only when nothing
    is decoding — exactly when a nudge helps. So tune for *how long to let the user
    struggle before helping* (moderate, not maximal — a nudge at ~20 in the 32-run case
    would have helped sooner), traded against not nagging on a brief normal gap.
  - Run length tracks decode quality (good focus → runs of 1–2 at ~8–10 decodes/s;
    marginal/too-far → 24–32). Consider expressing "stuck" as **time-since-last-decode**
    (no decode for N seconds) rather than a raw frame-count, which is pipeline-rate-
    dependent. Decide in Step 4 when Python owns the policy.
- **Ring sizing.** With transport dedup, NEW(payload) events are rare; the ring mostly
  carries tiny markers, so it can be small. MISS is not deduped, but each MISS event is
  tiny (no payload) and the sustained-MISS scenario is transient. Confirm bounds under
  animated decode.
- **NOTHING transition dedup.** Only enqueue NOTHING on *transition*, not every idle
  frame, so the ring isn't flooded while the camera sees an empty scene. (MISS is
  explicitly excluded from this — see §5.)
- **`qr_overlay_test` (disposable PoC).** Uses the old in-task callback API; a
  board_common API change breaks its build. Either give it a small poll update to keep
  it building until the MicroPython integration is verified, or retire it early. It is
  meant to be removed (`git rm`) once Step 2/3 are verified — keep it un-entangled.
- **`scan_coord_test` (board_common C reference app).** Update to the poll API; it is
  the C-side reference and should demonstrate the production shape.
- **QR corner coordinates on MISS → on-screen detection box (tiered).** A MISS means
  quirc *located* the code — `cam_pipeline_qr.h` documents `CAM_QR_MISS` as "QR located
  (corners valid) but decode failed," and k_quirc exposes `k_quirc_point_t corners[4]`.
  The data is already computed, but NOT plumbed to the MISS path today (the callback's
  `k_quirc_data_t *metadata` is valid only on DECODED; corners live in the
  identify/extract product, not the decode product). So three decoupled tiers:
  1. **Consecutive-MISS counter → warning UI** — pure Python over the preserved MISS
     stream. No engine/overlay change. First MISS consumer; build with tier-1 of the
     sustained-miss warning above.
  2. **Reserve the contract slot NOW** (cheap, do it): the C MISS event struct carries
     `bool has_corners; k_quirc_point_t corners[4]` (false/empty until populated), and
     the Python `poll()` returns a **structured event object, NOT a positional tuple**,
     so a `corners` field is purely additive later. Rationale: the expensive thing to
     change later is the C↔Python contract boundary, not engine internals — shape it to
     grow from the start.
  3. **Populate + render (defer):** an esp-camera-pipeline engine change to surface
     corners on the MISS path, overlay quad-rendering in lvgl-screens, and the
     **sensor-frame → cropped/scaled preview-square → overlay/display coordinate
     transform**. Defer until (a) MISS confirmed relevant on device and (b) a renderer
     exists to validate against — the coord transform is easy to get subtly wrong with
     no on-screen box to check it. Build the transform alongside the renderer.
- **Runner arity fix (separate, pre-existing).** `lvgl_screen_runner.py` calls
  1-arg native screen fns with `()` when `attrs is None` → "function takes 1 positional
  argument but 0 given". Fold the optional-cfg fix into the Step 2 binding pass.

---

## 7a. Backpressure & overflow (when decode outruns Python)

The decode task can enqueue faster than the MicroPython consumer drains. The ring
drops oldest-first, so a lagging consumer grabs a recent-ish event and loses the
intervening ones. That is SAFE only because of an asymmetry — and the model is split
to honor it:

**Not all events are equal under drop:**
- **REPEAT / NOTHING** are status-like (latest-wins). Dropping intermediates is
  harmless — same idempotent-snapshot logic as the outbound overlay.
- **NEW(payload)** is unique accumulation data. Dropping a NEW = **lost scan
  progress** (a missed part), NOT cosmetic lag. This is the only place in the
  pipeline where a drop loses information. Safe-but-slower: a dropped part only
  *delays* completion — and how little depends on the format: **UR** is a rateless
  fountain stream (no repeating cycle; a dropped part isn't re-acquired specifically,
  the decoder just needs *enough total* parts, which keep arriving — extremely
  drop-tolerant), while **BBQr** is a fixed repeating sequence (a dropped part costs
  ~one cycle until it comes back around). `DecodeQR` completes solely
  on a checksum-valid assembly, so a drop can never corrupt the result — it lags,
  it can't lie.
- **MISS** must not lose its *count* (sustained MISS = the "found but unreadable"
  signal, §5/§7).

**Therefore the channel is split by semantics (refines §4):**
- **NEW(payload) → small FIFO ring** — the precious, unique, SLOW stream. NEW is
  gated at the *decode-success* rate (~3.5–5/s measured for animated QR), NOT the
  camera rate (~13–15 fps), because a NEW only exists when a frame actually decoded.
  Size with a few parts' headroom; a NEW drop is **counted/logged, never silent**
  (no-silent-truncation principle).
- **REPEAT / NOTHING → one coalesced "latest frame status" cell** — latest-wins, no
  ring; dropping intermediates is correct by construction.
- **MISS → a consecutive-miss counter** (`consecutive_misses`), not discrete queued
  events. Increments on MISS; **reset by ANY non-MISS outcome** — a decode (NEW/REPEAT)
  because it proves readability, and NOTHING because "no QR located" ends the attempt
  (a reposition / switch to another code re-accumulates fresh). The reset MUST live in
  the coordinator: a valid QR held after it decoded streams REPEATs (no more NEWs) with
  stray MISS, and the consumer's coarse poll can't see every REPEAT to reset on — only
  the coordinator sees every frame. This sidesteps marker flooding AND makes the
  sustained-miss signal **undroppable** — the consumer just reads the current value and
  thresholds (`> 10`), requiring it to persist a couple polls. (Supersedes the earlier
  "monotonic count / consumer tracks the delta" framing — see §7 for why the device
  data forced this: MISS is the *normal* outcome during animated scanning, so the
  signal is miss-WITHOUT-progress, which a decode-reset counter expresses directly.)

**Consumer rule:** drain to empty each loop (poll until the NEW ring is empty + read
the status cell + `consecutive_misses`), not one-event-per-iteration, so it naturally
catches up. Overflow of NEW should not occur unless `add_data` sustains >~200 ms in
MicroPython — a number to **measure in Step 3**, not an architectural risk.

Net: backpressure can only ever discard redundant status; the precious NEW stream is
also the slowest; the MISS-persistence signal is a count that can't be dropped.

## 8. Proposed API sketch (for review)

```c
/* board_common: scan_coordinator (poll-based) */

typedef enum {
    SCAN_EVENT_NOTHING = 0,  /* nothing in frame                         */
    SCAN_EVENT_MISS,         /* located, not decoded                     */
    SCAN_EVENT_REPEAT,       /* byte-identical to last forwarded payload */
    SCAN_EVENT_NEW,          /* new raw payload (bytes attached)         */
} scan_event_kind_t;

/* present()/on_complete() are injected, fire on the CONSUMER's task. */
typedef void (*scan_present_fn)(void *ctx, int percent, scan_frame_status_t status);
typedef void (*scan_complete_fn)(void *ctx);

scan_coordinator_t *scan_coordinator_create(const scan_coordinator_config_t *cfg);
void scan_coordinator_destroy(scan_coordinator_t *);

/* Split channel (see §7a). NEW(payload) is the precious, unique, slow stream —
 * a small FIFO ring. REPEAT/MISS/NOTHING are coalesced status; MISS is a counter
 * so its signal can't be dropped. */

/* NEW events: drain one; payload valid until the next poll_new(). false when empty. */
typedef struct { const uint8_t *payload; size_t len; } scan_new_event_t;
bool scan_coordinator_poll_new(scan_coordinator_t *, scan_new_event_t *out);

/* Coalesced frame status + counters (latest-wins). Read once per loop.
 * consecutive_misses: run of MISS frames, reset by any non-MISS outcome (decode or
 * NONE); the consumer thresholds it.
 * `corners` is the LATEST miss location (the box only needs where it is NOW);
 * reserved for tier-2 forward-compat — has_corners=false until the engine plumbs
 * them (§7). A struct, not loose out-params, so it can grow without breaking
 * call sites. */
typedef struct {
    scan_frame_status_t latest;             /* REPEAT / MISS / NOTHING               */
    uint32_t            consecutive_misses;  /* run of MISS frames; any non-MISS resets it */
    uint32_t            dropped_new;          /* monotonic NEW parts dropped on overflow */
    bool                has_corners;          /* latest-MISS corners present (engine, later) */
    k_quirc_point_t     corners[4];           /* latest MISS location, when has_corners */
} scan_status_t;
void scan_coordinator_read_status(scan_coordinator_t *, scan_status_t *out);

/* consumer task: report the domain result of a NEW event back; coordinator
 * dedups on (status, percent) and dispatches present()/on_complete(). */
void scan_coordinator_report(scan_coordinator_t *, scan_frame_status_t status,
                             int percent);
```

```python
# builder MicroPython binding — the consumer's sole API.
# Structured returns (objects, not tuples) so fields can grow without breaking
# call sites (§7 tier 2). Drain the precious NEW ring to empty, THEN read status.
scanner.start()
while not decoder.is_complete:
    while (ev := scanner.poll_new()) is not None:   # drain NEW ring fully each loop
        status, pct = decoder.add_data(ev.payload)
        scanner.report(status, pct)                 # → present() → overlay under lock

    st = scanner.read_status()                      # coalesced status + counters
    if st.latest == REPEAT:
        scanner.report(PART_EXISTING, decoder.percent)   # gray dot, no classify
    # consecutive_misses resets on any non-MISS frame (decode or NONE, coordinator-side),
    # so it is miss-WITHOUT-progress by construction — just threshold the current value:
    if st.consecutive_misses >= MISS_WARN_THRESHOLD:  # e.g. 10, require persistence
        warn_found_but_unreadable()
        # future: + st.corners → draw detection box once engine plumbs them
    # NOTHING (st.latest) → hide dot
scanner.stop()
```
