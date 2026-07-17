"""Consolidated instrumentation-run sequencer (docs/instrumentation-run-spec.md).

One firmware, three run profiles: RUN_M5 (mounted, Sparrow 5 fps, 17 slots),
RUN_M10 (mounted, Sparrow 10 fps, the S/A/K block verbatim, zero-touch) and
RUN_H (hand-held, 4 slots). Every experimental dimension is a default-off
runtime knob staged per slot via camera_scanner.set_instr(); the sequencer
restarts the camera pipeline between slots (each restart also logs a fresh AE
settling transient at full rate).

Boot pattern (assess_scan.py lineage): this module is FROZEN into the
instrumentation firmware; /main.py is just

    import instrumented_run
    instrumented_run.main()

The run profile comes from /sd/instr_run.json when present, else from an
on-display tap menu. Everything lands under /sd/instr_<board>_r<N>/:

    run.log                      banners + mirrored 2 s stats blocks
    header.json                  profile, fps, config table, notes field
    <SCENARIO>/decode.csv        one row per decoder dispatch
    <SCENARIO>/gate.csv          one row per examined frame
    <SCENARIO>/frames/*.pgm      captured frames (miss/blend/decoded/nothing/burst)
    <SCENARIO>/frames/index.csv  capture metadata
    <SCENARIO>/counters.json     producer-side drop audit + fragment counters
    <SCENARIO>/bench.json        (K slots) per-trial completion times

Operator contract per the spec: slots 1-12 of M5 (and all of M10) are
zero-touch after the initial aim+tap; the M5 tail idles on a tap before each
scene change. UR completion never ends a slot; BENCHMARK slots (K1/K2) run
scan-to-completion trials instead of a fixed-duration shadow capture.

/sd/instr_run.json keys (all optional):
    profile       "M5" | "M10" | "H" | "D" | "HP"   (else tap menu; D = 3.5
                  dual re-bench: S1 stock / A1D DUAL shadow / K1 stock / KD
                  DUAL; HP = the H hand-held slots + the big-print paper G1)
    board         "43" (default) | "35"
    run_n         int  (else auto-increment)
    sparrow_fps   int  (recorded in header; when ABSENT the sequencer now
                  prompts per run -- the old profile-derived default wrote
                  stale values into 10 fps run headers)
    slot_seconds  int  (default 45)
    long_a_slots  bool (A1/A4/A5 at 90 s -- "blends too rare" fallback)
    burst_a1      bool (serial-burst capture class during A1; default off)
    notes         str  (copied into header.json)
"""
import gc
import json
import os
import sys
import time

import camera_scanner as cam
import seedsigner_lvgl_screens as ss

# Round-2 build: menu tuple fix + dual-decoder gating on the 3.5 + per-run fps
# prompt + HEALTH static-slot exemption. The dec480 suffix is derived from the
# firmware itself (camera_scanner.DECODE_480 == 1 on the 480x480-decode build
# variant), so both variants get distinguishable run headers from one source.
BUILD_TAG = "instr-run-r2dual+bbqrbench 2026-07-17" + (
    "+dec480" if getattr(cam, "DECODE_480", 0) else "")

CSV_DECODE_HEADER = ("run_id,scenario_id,config_id,seq,ts_us,decoder_id,"
                     "frame_gen,content_seq,outcome,passes,max_capstones,"
                     "seed_offset,locked_offset,win_offset,used_local,"
                     "blend_score,bailed_blend,bailed_noqr,decode_ms,"
                     "dispatch_age_ms,peer_busy,peer_depth,side_px,sharpness,"
                     "luma,dark_lobe_peak\n")
CSV_GATE_HEADER = ("run_id,scenario_id,config_id,ts_us,frame_gen,content_seq,"
                   "hamming_vs_last,gap_gens,stable_ms,decision,"
                   "dark_lobe_peak\n")
CAP_INDEX_HEADER = ("cls,seq,dispatch_seq,ts_us,w,h,decoder_id,outcome,"
                    "blend_score,side_px,sharpness,luma,file\n")
CAP_CLS_NAMES = ("miss", "blend", "decoded", "nothing", "burst")

# ── Config table (instrumentation-run-spec §4). Values are set_instr kwargs;
# instr_log is added at slot start. BASE == deployed behavior. ──────────────
CONFIGS = {
    "BASE":     {},
    "SOLO":     {"num_decoders": 1},
    # DUAL: force the 2nd decoder on boards whose scan-start default is single
    # (the 3.5 after its LVGL core was freed; camera_scanner's portrait-only
    # gating predates that reconfig). No-op on the 4.3 portrait path.
    "DUAL":     {"num_decoders": 2},
    "CAP3":     {"sweep_cap": 3},
    "GATED":    {"blend_gate": 130},  # CP-1-validated 4.3 threshold ONLY
    "DEDUP":    {"dedup": True},
    "DEB40":    {"debounce_ms": 40},
    "SEED15":   {"seed_pin": -15, "lock_freeze": True},
    "STK_FULL": {"num_decoders": 1, "hash_gate": False, "fixed_threshold": True},
    "STK_DEC":  {"fixed_threshold": True},
    "THOR_S":   {"thorough": True},
    "THOR_A":   {"thorough": True, "ladder": 1},
}

# Slot tuples: (scenario_id, config_id, mode, prompt)
#   mode: "shadow" (fixed duration) | "bench" (scan-to-completion trials)
#   prompt: None = zero-touch; else operator text, tap-gated before the slot.
_SAK_BLOCK = [
    ("S1", "STK_FULL", "shadow", None),
    ("S2", "STK_DEC", "shadow", None),
    ("A1", "BASE", "shadow", None),
    ("A2", "SOLO", "shadow", None),
    ("A3", "CAP3", "shadow", None),
    ("A4", "GATED", "shadow", None),  # 4.3 only; dropped for board 35
    ("A5", "BASE", "shadow", None),
    ("A6", "DEDUP", "shadow", None),
    ("A7", "DEB40", "shadow", None),
    ("A8", "SEED15", "shadow", None),
    ("K1", "STK_FULL", "bench", None),
    ("K2", "BASE", "bench", None),
]
_M5_TAIL = [
    ("C1", "BASE", "shadow", "Set laptop display brightness LOW"),
    ("F1", "THOR_S", "shadow", "Restore brightness; show STATIC QR on screen"),
    ("F2", "THOR_A", "shadow", "Keep the same static QR on screen"),
    ("G1", "THOR_S", "shadow", "Show the static PAPER QR"),
    ("E1", "BASE", "shadow", "SMUDGE the lens (fingerprint/breath); animated scene"),
]
# Static-media slots: the scene is ONE unchanging QR, so transport dedup
# correctly yields a single unique fragment. The generic "seen >= 3" HEALTH
# heuristic below is a false positive there (every F/G slot flagged LIKELY-BAD
# in the r3/r4 datasets); judge them on "decoded at least once" instead.
STATIC_SLOTS = ("F1", "F2", "G1")

_H_BLOCK = [
    ("H1", "BASE", "shadow", "HAND-HELD: standard scene, hold steady-ish"),
    ("H2", "DEB40", "shadow", "HAND-HELD: same scene"),
    ("H3", "BASE", "shadow",
     "HAND-HELD AIMING: start pulled back ~50% fill, approach, settle, re-aim"),
    ("H4", "DEB40", "shadow", "HAND-HELD: deliberately unsteady, exaggerated motion"),
]

# B: BBQR-cycle completion bench, HAND-HELD, stability-first (see
# docs/bbqr-bench-plan.md). Source = the SAME 30-input tx, but shown as an
# animated BBQr (Sparrow: Show PSBT as QR -> Show BBQr) dialed to LOW density
# (the only density the camera decodes). BBQr is a fixed cyclic sequence, so a
# located-miss costs a whole cycle -- this bench is built to expose that gap.
# Three arms A/B stock vs PR #11 vs the blend gate on the identical scene:
#   BSTK  = stock proxy (fixed-threshold decode, no sweep, single decoder)
#   BBASE = PR #11 adaptive sweep, blend gate in SHADOW (no bail)
#   BGATE = BBASE + blend gate ARMED (4.3 only; dropped on the 3.5 per CP-1)
# Each arm runs BENCH_TRIALS cold-start trials (COLD_LOCK_SLOTS resets the lock
# per trial). The 3.5 is a go/no-go: its AIM+VERIFY gate confirms whether the
# lower-res path decodes low-density BBQr at all before any data is captured.
_BBQR_BLOCK = [
    ("BSTK",  "STK_FULL", "bench", "HAND-HELD BBQr (LOW density): hold steady. STOCK decode"),
    ("BBASE", "BASE",     "bench", "HAND-HELD BBQr: hold steady. PR#11 adaptive sweep"),
    ("BGATE", "GATED",    "bench", "HAND-HELD BBQr: hold steady. PR#11 + blend gate"),
]

RUN_TABLES = {
    "M5": _SAK_BLOCK + _M5_TAIL,
    "M10": list(_SAK_BLOCK),
    "H": list(_H_BLOCK),
    # HP: the hand-held sweep plus the big-print paper retry. G1 goes LAST so
    # the media switch (Sparrow screen -> paper) happens once. The data4 paper
    # failure was FOCUS COLLAPSE (sharpness 126 vs ~726 on-LCD), so the paper
    # prompt asks the operator to hunt a sharp distance, not fill the frame.
    "HP": _H_BLOCK + [
        ("G1", "THOR_S", "shadow",
         "BIG-print PAPER QR, hand-held at arm's length: find SHARP focus, "
         "distance over fill"),
    ],
    # D: dual-decoder re-bench for the 3.5 (mounted; needs a build where the
    # freed-LVGL-core reconfig is present). Stock reference + DUAL arms.
    "D": [
        ("S1", "STK_FULL", "shadow", None),
        ("A1D", "DUAL", "shadow", None),
        ("K1", "STK_FULL", "bench", None),
        ("KD", "DUAL", "bench", None),
    ],
    "B": list(_BBQR_BLOCK),
}
BENCH_TRIALS = 3
BENCH_SLOT_CAP_S = 150
BENCH_TRIAL_CAP_S = 90   # safety reset: a single trial running this long w/o
                         # completing is abandoned (bad aim / non-assembling)
# BBQR hand-held bench (profile B): tighter caps so the STK/BASE/GATED trio at
# two fps stays ~8-10 min of holding. Retune after run 1 via instr_run.json
# (bench_slot_cap_s / bench_trial_cap_s). BBQR completions are short when
# decoding well (~1-3 low-density cycles); a stalled arm is the signal, capped.
BBQR_SLOT_CAP_S = 120
BBQR_TRIAL_CAP_S = 60
# Bench scenarios that cold-start the offset lock at each trial (so the 3 trials
# are independent cold-start samples, not 1 cold + 2 warm). The UR K-slots are
# deliberately NOT here -- they keep the warm-lock behavior of the existing
# corpus so their numbers stay comparable.
COLD_LOCK_SLOTS = ("BSTK", "BBASE", "BGATE")

# Per-slot frame-capture budget. The SD write path is slow on this rig
# (~1.2-1.8 s per 225 KB PGM), so writing the ~58 frames/slot the rate caps
# would produce takes ~70-100 s and inflates every 45 s slot to ~140 s. Cap the
# number of frames actually written per slot so the SD load fits inside the
# slot; the rest of the eligible frames are dropped (a sample is all the
# corpora need). ~12 frames = ~18 s of writes, comfortably inside 45 s.
FRAME_CAP = 12


def _detect_board():
    """Map the firmware's board name (os.uname().machine, from
    MICROPY_HW_BOARD_NAME) to the run's board id, so the sequencer picks the
    right per-board defaults (the 3.5 skips slot A4 and writes instr_35_*)
    WITHOUT a config file -- the tap menu then works identically on both
    boards. A "board" key in instr_run.json still overrides this."""
    try:
        m = os.uname().machine
    except Exception:
        return "43"
    if "3.5" in m:
        return "35"
    if "4.3" in m:
        return "43"
    return "43"


def _mount_sd():
    try:
        os.stat("/sd")
        return
    except OSError:
        pass
    import machine
    import vfs
    sd = machine.SDCard(slot=0, width=4)
    vfs.mount(vfs.VfsFat(sd), "/sd")


def _load_run_config():
    try:
        with open("/sd/instr_run.json") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def _tap_prompt(title, buttons):
    """Blocking menu: render a button list, wait for a tap, return the index."""
    ss.clear_result_queue()
    ss.button_list_screen({
        "top_nav": {"title": title[:14], "show_back_button": False,
                    "show_power_button": False},
        "button_list": buttons,
    })
    while True:
        ev = ss.poll_for_result()
        if ev is not None:
            # poll_for_result returns (kind, index, label). Comparing the raw
            # tuple against int indices made every menu silently fall through
            # to its default (the "tapped RUN H, got the M5 table" bug) --
            # normalize to the int index here.
            if isinstance(ev, tuple):
                return int(ev[1])
            return ev
        time.sleep_ms(50)


def _tap_gate(title, instruction):
    """Operator gate: instruction rendered as the button labels; any tap
    proceeds. The sequencer idles here until the operator returns."""
    ss.clear_result_queue()
    # Split the instruction across button-length lines, last button = GO.
    words = instruction.split()
    lines, cur = [], ""
    for w in words:
        if len(cur) + len(w) + 1 > 24:
            lines.append(cur)
            cur = w
        else:
            cur = (cur + " " + w).strip()
    if cur:
        lines.append(cur)
    ss.button_list_screen({
        "top_nav": {"title": title[:14], "show_back_button": False,
                    "show_power_button": False},
        "button_list": lines[:6] + ["TAP TO START"],
    })
    while ss.poll_for_result() is None:
        time.sleep_ms(50)


class SlotWriter(object):
    """Per-scenario SD writers: decode.csv, gate.csv, frames/, index.csv."""

    def __init__(self, run_dir, run_id, scenario, config_id):
        # Collision-proof: if this scenario already captured into this run dir
        # (a mid-sequence redo, or a pinned run_n), suffix __2/__3/... instead
        # of overwriting, so a retry never destroys an earlier attempt.
        name = scenario
        n = 1
        while True:
            cand = "%s/%s" % (run_dir, name)
            try:
                os.stat(cand + "/decode.csv")
                n += 1
                name = "%s__%d" % (scenario, n)
            except OSError:
                break
        self.dir = "%s/%s" % (run_dir, name)
        self.scenario_dir = name
        try:
            os.mkdir(self.dir)
        except OSError:
            pass
        try:
            os.mkdir(self.dir + "/frames")
        except OSError:
            pass
        self.prefix = "%s,%s,%s," % (run_id, scenario, config_id)
        self.decode_f = open(self.dir + "/decode.csv", "w")
        self.decode_f.write(CSV_DECODE_HEADER)
        self.gate_f = open(self.dir + "/gate.csv", "w")
        self.gate_f.write(CSV_GATE_HEADER)
        self.idx_f = open(self.dir + "/frames/index.csv", "w")
        self.idx_f.write(CAP_INDEX_HEADER)
        self.n_frames = 0
        self.ur_parts_f = None  # lazily opened in bench slots
        self.cur_err_n = 0
        self.ur_part_n = 0

    def record_ur_part(self, payload):
        """Bench slots: log EVERY UR payload fed to the decoder (one raw line
        each), so the exact fountain sequence is replayable offline -- both to
        reproduce a cUR failure byte-for-byte and to see why a completion is
        never detected. Returns the 1-based part index."""
        self.ur_part_n += 1
        try:
            if self.ur_parts_f is None:
                self.ur_parts_f = open(self.dir + "/ur_parts.log", "wb")
            b = payload if isinstance(payload, (bytes, bytearray)) \
                else str(payload).encode()
            self.ur_parts_f.write(b)
            self.ur_parts_f.write(b"\n")
            # NO per-part flush: a flush forces an SD sync (~50-150 ms here),
            # and at ~5 parts/s that starves the consumer loop enough to drop
            # fountain parts. The file is flushed on close (finish()); a hard
            # power-off loses at most the last buffered lines, acceptable.
        except Exception:
            pass
        return self.ur_part_n

    def record_cur_error(self, payload, exc, note=""):
        """Bench slots: on a decoder error, persist the offending fragment
        (raw bytes, for byte-exact replay) + the full traceback + context, so
        cUR can be patched from the real failing input. Cheap and bounded --
        one small .bin per error + appended log lines."""
        self.cur_err_n += 1
        n = self.cur_err_n
        try:
            os.mkdir(self.dir + "/cur_errors")
        except OSError:
            pass
        try:
            b = payload if isinstance(payload, (bytes, bytearray)) \
                else str(payload).encode()
            with open("%s/cur_errors/frag_%03d.bin" % (self.dir, n), "wb") as bf:
                bf.write(b)
        except Exception:
            pass
        try:
            with open(self.dir + "/cur_errors.log", "a") as lf:
                lf.write("=== cUR error #%d note=%s part_index=%d ===\n"
                         % (n, note, self.ur_part_n))
                try:
                    lf.write("payload_len=%d\npayload=%r\n"
                             % (len(payload), bytes(payload[:240])))
                except Exception:
                    pass
                try:
                    sys.print_exception(exc, lf)
                except Exception:
                    lf.write("exc=%r\n" % (exc,))
                lf.write("\n")
        except Exception:
            pass

    def drain_csv(self):
        for which, f in ((0, self.decode_f), (1, self.gate_f)):
            for _ in range(8):  # bounded per tick
                chunk = cam.poll_instr_csv(which)
                if chunk is None:
                    break
                text = chunk.decode()
                for line in text.split("\n"):
                    if line:
                        f.write(self.prefix)
                        f.write(line)
                        f.write("\n")

    def drain_captures(self, max_per_tick=1):
        # Per-slot budget reached: don't drain/write any more (the slow SD
        # writes are what inflate the slot). The decode task's excess captures
        # just fill the ring and drop -- counted as capture_drops, harmless.
        if self.n_frames >= FRAME_CAP:
            return
        for _ in range(max_per_tick):
            r = cam.poll_instr_capture()
            if r is None:
                return
            data, m = r
            name = "%s_%04d.pgm" % (CAP_CLS_NAMES[m.cls], m.seq)
            path = "%s/frames/%s" % (self.dir, name)
            with open(path, "wb") as pf:
                pf.write(("P5\n%d %d\n255\n" % (m.width, m.height)).encode())
                pf.write(data)
            self.idx_f.write("%d,%d,%d,%d,%d,%d,%d,%d,%d,%.0f,%.1f,%d,%s\n" % (
                m.cls, m.seq, m.dispatch_seq, m.timestamp_us, m.width,
                m.height, m.decoder_id, m.outcome, m.blend_score, m.side_px,
                m.sharpness, m.luma, name))
            self.n_frames += 1
            data = None
            if self.n_frames >= FRAME_CAP:
                return

    def finish(self):
        c = cam.instr_counters()
        with open(self.dir + "/counters.json", "w") as f:
            json.dump({
                "decode_rows": c.decode_rows, "decode_drops": c.decode_drops,
                "gate_rows": c.gate_rows, "gate_drops": c.gate_drops,
                "captures": c.captures, "capture_drops": c.capture_drops,
                "frag_arrived": c.frag_arrived,
                "frag_dispatched": c.frag_dispatched,
                "frag_expired": c.frag_expired,
                "frames_written": self.n_frames,
            }, f)
        self.decode_f.close()
        self.gate_f.close()
        self.idx_f.close()
        if self.ur_parts_f is not None:
            try:
                self.ur_parts_f.close()
            except Exception:
                pass
            self.ur_parts_f = None


class RunLog(object):
    def __init__(self, path):
        # Append, so re-running into an existing run dir (pinned run_n) never
        # truncates an earlier session's log.
        self.f = open(path, "a")

    def line(self, text):
        print(text)
        self.f.write(text)
        self.f.write("\n")
        self.f.flush()

    def mirror_stats(self):
        block = cam.poll_instr_stats()
        if block:
            self.f.write(block)
            self.f.write("\n")
            self.f.flush()

    def close(self):
        self.f.close()


def _drain_new_parts(decoder=None, writer=None):
    """Drain the NEW ring. Shadow slots discard (telemetry captures
    everything); bench slots feed the app's DecodeQR. Returns True when the
    decoder just completed.

    Robustness: a stray/garbled fragment can make DecodeQR.add_data raise
    (e.g. a transient 'QR Fragment Unexpected Type Change'). That must NEVER
    propagate -- one bad frame would otherwise kill the whole run. Completion
    is read from add_data's RETURN value (DecodeQRStatus.COMPLETE == 3),
    entirely inside the try, so both the parse and the completion check are
    guarded. This mirrors what the AIM-verify loop already does."""
    for _ in range(16):
        p = cam.poll_new()
        if p is None:
            return False
        if decoder is not None:
            if writer is not None:
                writer.record_ur_part(p)  # full replayable sequence
            try:
                if decoder.add_data(p) == 3:  # DecodeQRStatus.COMPLETE
                    return True
            except Exception as e:
                sys.print_exception(e)
                # Persist the exact failing fragment + traceback so cUR can be
                # patched from real input; then skip it and keep going.
                if writer is not None:
                    writer.record_cur_error(p, e, "add_data")
                continue
    return False


def _run_shadow_slot(log, writer, duration_s):
    """Fixed-duration shadow capture. Also drives the on-screen dot/bar from
    raw decode activity (no DecodeQR needed) so a PRESENT operator can see the
    slot is decoding -- the dot flashes GREEN on new fragments, shows the MISS
    state when a QR is located but not decoding (the classic bad-focus tell),
    and stays empty when nothing is found. Zero-touch slots ignore it; it costs
    nothing and never touches the captured telemetry (that comes from the
    decode tasks regardless)."""
    t0 = time.ticks_ms()
    total_ms = duration_s * 1000
    seen = 0
    while True:
        el = time.ticks_diff(time.ticks_ms(), t0)
        if el >= total_ms:
            break
        # Bar = how far through this slot (real, orienting progress); dot =
        # decode health (green on a fresh decode, MISS when a QR is located but
        # not decoding, empty otherwise). report() dedups on (status, pct), so
        # the bar only redraws ~1%/step and this is not spammy.
        pct = el * 100 // total_ms
        if pct > 99:
            pct = 99
        got = False
        for _ in range(16):
            p = cam.poll_new()
            if p is None:
                break
            got = True
            seen += 1
        if got:
            cam.report(cam.FRAME_NEW, pct)          # green: decoding
        else:
            st = cam.read_status()
            cam.report(cam.FRAME_MISS if st.latest == cam.FRAME_MISS
                       else cam.FRAME_NONE, pct)    # located-not-decoding / idle
        writer.drain_csv()
        writer.drain_captures()
        log.mirror_stats()
        time.sleep_ms(25)
    return seen


def _run_bench_slot(log, writer, slot_cap_s=BENCH_SLOT_CAP_S,
                    trial_cap_s=BENCH_TRIAL_CAP_S, cold_lock=False):
    """BENCHMARK mode: full scan-to-completion trials -- reset the app decoder,
    scan until the standard tx completes (UR fountain or BBQr cycle -- DecodeQR
    auto-detects), log elapsed, repeat -- up to BENCH_TRIALS completions or
    slot_cap_s, whichever first. A trial that runs trial_cap_s without completing
    is abandoned (safety reset).

    cold_lock=True cold-starts the decoder's adaptive-threshold lock at the top
    of every trial (cam.reset_lock()), so the 3 trials are independent cold-start
    samples including the realistic lock-acquisition cost -- used for the BBQr
    bench. It touches only the offset lock, not the camera/AE."""
    from seedsigner.models.decode_qr import DecodeQR

    def _cold():
        # Guarded: reset_lock is present in this firmware, but never let a
        # missing binding take down a bench slot.
        if cold_lock:
            try:
                cam.reset_lock()
            except Exception:
                pass

    trials = []
    resets = 0
    slot_t0 = time.ticks_ms()
    _cold()                       # trial 1 cold-start
    decoder = DecodeQR()
    trial_t0 = time.ticks_ms()
    last_pct_log = slot_t0
    max_pct = 0
    try:
        while (time.ticks_diff(time.ticks_ms(), slot_t0) < slot_cap_s * 1000
               and len(trials) < BENCH_TRIALS):
            done = _drain_new_parts(decoder, writer)
            # Drive the overlay so a benchmark slot isn't a static/"frozen"
            # screen: green dot + bar = THIS fountain's assembly progress toward
            # completion; it snaps to 100 on a completion then resets to 0 for
            # the next trial. report() dedups on (status, pct) so this is cheap.
            try:
                cam.report(cam.FRAME_NEW, decoder.get_percent_complete())
            except Exception:
                pass
            writer.drain_csv()
            # NO writer.drain_captures() here: the ~1.5 s PGM SD writes block
            # the loop long enough to drop fountain parts, which is exactly what
            # prevented completion. The benchmark measures time-to-complete and
            # doesn't need a frame corpus; the shadow slots provide that.
            log.mirror_stats()
            now = time.ticks_ms()
            if done:
                dt = time.ticks_diff(now, trial_t0)
                trials.append(dt)
                log.line("[bench] trial %d COMPLETE in %d ms" % (len(trials), dt))
                _cold()               # next trial cold-start
                decoder = DecodeQR()
                gc.collect()
                trial_t0 = now
                max_pct = 0
            elif time.ticks_diff(now, trial_t0) > trial_cap_s * 1000:
                # Safety reset: a trial running trial_cap_s without completing
                # (the tx not assembling, bad aim, or a decoder that won't
                # finish) must not accumulate unbounded -- reset so we get a
                # clean retry and bound memory. Logged so the analysis sees it.
                resets += 1
                log.line("[bench] trial RESET after %ds, no completion "
                         "(max %%=%d, parts=%d)"
                         % (trial_cap_s, max_pct, writer.ur_part_n))
                _cold()               # retry cold-start
                decoder = DecodeQR()
                gc.collect()
                trial_t0 = now
                max_pct = 0
            else:
                # Progress probe: why isn't it completing? Log the decoder's
                # percent every 5 s (guarded -- get_percent_complete can raise
                # on a partially-parsed fountain). Rising-but-never-100 vs
                # stuck-at-low both diagnose the no-completion.
                if time.ticks_diff(now, last_pct_log) >= 5000:
                    try:
                        pct = decoder.get_percent_complete()
                        if pct > max_pct:
                            max_pct = pct
                        log.line("[bench] progress: %d%% (max %d) parts=%d"
                                 % (pct, max_pct, writer.ur_part_n))
                    except Exception as e:
                        writer.record_cur_error(b"(get_percent_complete)", e,
                                                "percent")
                    last_pct_log = now
            time.sleep_ms(25)
    finally:
        # Always write bench.json, even if the loop raised, so the slot's
        # result is never lost.
        try:
            with open(writer.dir + "/bench.json", "w") as f:
                json.dump({"trial_ms": trials, "completions": len(trials),
                           "resets": resets, "max_percent": max_pct,
                           "ur_parts_fed": writer.ur_part_n,
                           "cur_errors": writer.cur_err_n,
                           "cap_s": slot_cap_s, "trial_cap_s": trial_cap_s,
                           "cold_lock": bool(cold_lock)}, f)
        except Exception as e:
            sys.print_exception(e)
    if trials:
        srt = sorted(trials)
        log.line("[bench] median time-to-complete: %d ms (%d trials, %d resets, "
                 "%d cUR errors)" % (srt[len(srt) // 2], len(trials), resets,
                                     writer.cur_err_n))
    else:
        log.line("[bench] NO completion within %d s (max %%=%d, %d parts fed, "
                 "%d cUR errors -> see ur_parts.log / cur_errors/)"
                 % (slot_cap_s, max_pct, writer.ur_part_n, writer.cur_err_n))


def _slot_duration(scenario, cfgfile):
    base = int(cfgfile.get("slot_seconds", 45))
    if cfgfile.get("long_a_slots") and scenario in ("A1", "A4", "A5"):
        return 90
    return base


def _bench_caps(profile, cfgfile):
    """(slot_cap_s, trial_cap_s) for BENCHMARK slots. The BBQr hand-held bench
    uses tighter caps to bound holding time; both are overridable per run via
    instr_run.json (bench_slot_cap_s / bench_trial_cap_s)."""
    if profile == "B":
        slot_default, trial_default = BBQR_SLOT_CAP_S, BBQR_TRIAL_CAP_S
    else:
        slot_default, trial_default = BENCH_SLOT_CAP_S, BENCH_TRIAL_CAP_S
    return (int(cfgfile.get("bench_slot_cap_s", slot_default)),
            int(cfgfile.get("bench_trial_cap_s", trial_default)))


def main():
    print("[instr] %s starting; init display + SD ..." % BUILD_TAG)
    ss.init()
    try:
        ss.set_screensaver_timeout(0)
    except Exception:
        pass
    _mount_sd()
    # Chain runs in ONE boot: after each profile completes, return to the menu
    # with NO power-cycle -- so a mounted rig stays put (the Q7 same-sitting
    # requirement) and runs auto-number r1/r2/r3. "Finish" drops to the REPL.
    # (A power-cycle also works and gives the cleanest state; this is the
    # mount-safe convenience path.) The config file is re-read each loop, so an
    # SD edit between runs is picked up.
    while True:
        cfgfile = _load_run_config()
        _run_once(cfgfile)
        if _tap_prompt("RUN DONE",
                       ["Run another test (back to menu)",
                        "Finish (power off / REPL)"]) != 0:
            print("[instr] finished; data on /sd. Dropping to REPL.")
            return


def _run_once(cfgfile):
    profile = cfgfile.get("profile")
    if profile not in RUN_TABLES:
        idx = _tap_prompt("INSTR RUN",
                          ["RUN M5 (mounted, 5fps)",
                           "RUN M10 (mounted, 10fps)",
                           "RUN H (hand-held)",
                           "RUN D (dual re-bench)",
                           "RUN HP (hand-held + paper)",
                           "RUN B (BBQr hand-held bench)"])
        profile = ("M5", "M10", "H", "D", "HP", "B")[idx] \
            if idx in (0, 1, 2, 3, 4, 5) else "M5"

    board = str(cfgfile.get("board") or _detect_board())
    # sparrow_fps: pinned via /sd/instr_run.json, else CONFIRMED PER RUN with a
    # tap menu. (Stale-header bug: the old profile-derived default meant a
    # 10 fps capture session recorded whatever fps an earlier config file or
    # profile default said — the r3/r4 10 fps headers were all wrong and the
    # real speed had to be reverse-engineered from gate-CSV cadence.)
    _fps_cfg = cfgfile.get("sparrow_fps")
    if _fps_cfg is not None:
        sparrow_fps = int(_fps_cfg)
    else:
        _fps_default = 10 if profile in ("M10", "D") else 5
        _fps_idx = _tap_prompt("SPARROW FPS",
                               ["5 fps", "10 fps", "15 fps", "20 fps",
                                "profile default (%d fps)" % _fps_default])
        sparrow_fps = (5, 10, 15, 20, _fps_default)[_fps_idx] \
            if _fps_idx in (0, 1, 2, 3, 4) else _fps_default
    # 3.5 has no gated slot (CP-1 blur/blend overlap): drop every GATED-config
    # scenario on it -- A4 (mounted tables) and BGATE (the BBQr bench).
    _GATED_SLOTS = ("A4", "BGATE")
    slots = [s for s in RUN_TABLES[profile]
             if not (board == "35" and s[0] in _GATED_SLOTS)]

    # Lens-smudge slot (E1, Q10) is OPT-IN: it dirties the lens and is awkward
    # to clean on some rigs, so it is skipped unless include_smudge is set in
    # the config. A general "skip" list omits any other scenarios by id too.
    skip = set(cfgfile.get("skip", []))
    if not cfgfile.get("include_smudge"):
        skip.add("E1")
    if skip:
        slots = [s for s in slots if s[0] not in skip]

    # Optional subset selection (redo just some tests without the whole table):
    #   "slots":    ["A3", "A6"]  -> run only these scenario ids, in order
    #   "start_at": "A5"          -> skip everything before this scenario
    # Combine with a pinned "run_n" to append the redo into an existing run
    # dir (the writer suffixes __2/__3 so nothing is overwritten).
    only = cfgfile.get("slots")
    if only:
        only = set(only)
        slots = [s for s in slots if s[0] in only]
    start_at = cfgfile.get("start_at")
    if start_at:
        ids = [s[0] for s in slots]
        if start_at in ids:
            slots = slots[ids.index(start_at):]
    if not slots:
        print("[instr] no slots selected (check 'slots'/'start_at'); nothing to do")
        return

    # Run directory: auto-increment run_n unless pinned in the config file.
    run_n = cfgfile.get("run_n")
    if run_n is None:
        run_n = 1
        while True:
            try:
                os.stat("/sd/instr_%s_r%d" % (board, run_n))
                run_n += 1
            except OSError:
                break
    run_dir = "/sd/instr_%s_r%d" % (board, run_n)
    try:
        os.mkdir(run_dir)
    except OSError:
        pass
    run_id = "%s_%s_r%d" % (board, profile, run_n)

    log = RunLog(run_dir + "/run.log")
    log.line("=== INSTRUMENTATION RUN %s profile=%s board=%s sparrow=%dfps "
             "build='%s' ===" % (run_id, profile, board, sparrow_fps, BUILD_TAG))

    header = {
        "run_id": run_id, "profile": profile, "board": board,
        "sparrow_fps": sparrow_fps, "build": BUILD_TAG,
        "uname": list(os.uname()),
        "configs": CONFIGS,
        "slots": [{"scenario": s[0], "config": s[1], "mode": s[2],
                   "duration_s": (_bench_caps(profile, cfgfile)[0] if s[2] == "bench"
                                  else _slot_duration(s[0], cfgfile))}
                  for s in slots],
        "burst_a1": bool(cfgfile.get("burst_a1")),
        "long_a_slots": bool(cfgfile.get("long_a_slots")),
        "camera": "OV5647 MIPI RAW10 1280x960 binning 45fps (board default)",
        "source": "standard 30-input tx, endless UR fountain from Sparrow",
        "operator_notes": cfgfile.get("notes", ""),
    }
    # Non-clobbering: a redo appended into an existing run dir keeps the
    # original header (header.json), landing as header__2.json etc.
    hname = "header.json"
    hn = 1
    while True:
        try:
            os.stat(run_dir + "/" + hname)
            hn += 1
            hname = "header__%d.json" % hn
        except OSError:
            break
    with open(run_dir + "/" + hname, "w") as f:
        json.dump(header, f)

    # Initial AIM + VERIFY gate (all profiles): a LIVE DECODE check, not just a
    # preview. The operator watches the on-screen dot flash GREEN and the bar
    # climb (a real DecodeQR is fed, so bad focus/framing shows as no green /
    # a stalled bar) and only taps BACK to launch the sequence once the setup
    # is decoding well. Nothing is captured here -- so a bad setup is caught
    # and fixed BEFORE any slot data exists.
    _tap_gate("RUN " + profile,
              "AIM + VERIFY: next screen is a LIVE decode check. Watch the dot "
              "flash GREEN and the bar climb -- that means the setup is "
              "decoding. Re-aim until it's healthy, THEN tap BACK to start.")
    log.line("[instr] AIM+VERIFY: live decode; tap BACK only when decoding well")
    cam.set_instr()  # deployed behavior, no telemetry
    _verify_health = None
    try:
        from seedsigner.models.decode_qr import DecodeQR
        _verify_health = DecodeQR()
    except Exception as e:
        sys.print_exception(e)  # fall back to raw dot feedback below
    try:
        cam.start()
        ss.clear_result_queue()
        new_parts = 0
        misses = 0
        last_beat = time.ticks_ms()
        while ss.poll_for_result() is None:
            got = False
            for _ in range(16):
                p = cam.poll_new()
                if p is None:
                    break
                got = True
                new_parts += 1
                pct = min(99, new_parts * 3)
                if _verify_health is not None:
                    try:
                        # is_complete is a @property on DecodeQR (returns
                        # self.complete) -- NOT a method. Calling it as
                        # is_complete() raises TypeError('bool' not callable);
                        # here that was silently swallowed, so AIM never showed
                        # completions. Read the property, or the fresh add_data
                        # status (COMPLETE == 3).
                        if _verify_health.add_data(p) == 3 or \
                                _verify_health.is_complete:
                            _verify_health = DecodeQR()  # fountain looped; reset
                        pct = _verify_health.get_percent_complete()
                    except Exception:
                        pass
                cam.report(cam.FRAME_NEW, pct)  # dot GREEN + bar -> "decoding"
            if not got:
                st = cam.read_status()
                if st.latest == cam.FRAME_MISS:
                    misses += 1
                    cam.report(cam.FRAME_MISS, 0)  # located but NOT decoding
            now = time.ticks_ms()
            if time.ticks_diff(now, last_beat) >= 1000:
                print("[aim] decoded_parts=%d located-not-decoded=%d "
                      "(want the first number climbing steadily)"
                      % (new_parts, misses))
                last_beat = now
            time.sleep_ms(30)
    finally:
        cam.stop()
    log.line("[instr] AIM done: %d parts decoded, %d located-not-decoded during "
             "verify" % (new_parts, misses))

    bench_slot_cap, bench_trial_cap = _bench_caps(profile, cfgfile)
    for i, (scenario, config_id, mode, prompt) in enumerate(slots):
        dur = bench_slot_cap if mode == "bench" else _slot_duration(scenario, cfgfile)
        if prompt:
            log.line("[instr] waiting on operator: %s" % prompt)
            _tap_gate("%s %s" % (scenario, config_id), prompt)
        banner = ("=== SLOT %d/%d scenario=%s config=%s t=%ds mode=%s ==="
                  % (i + 1, len(slots), scenario, config_id, dur, mode))
        log.line(banner)

        kwargs = dict(CONFIGS[config_id])
        kwargs["instr_log"] = True
        if scenario == "H3":
            kwargs["capture_nothing"] = True  # aiming-regime corpus
        if scenario == "A1" and cfgfile.get("burst_a1"):
            kwargs["burst"] = True
        cam.set_instr(**kwargs)

        writer = SlotWriter(run_dir, run_id, scenario, config_id)
        try:
            cam.start()
        except OSError as e:
            log.line("[instr] cam start FAILED for %s: %s -- skipping" %
                     (scenario, e))
            writer.finish()
            continue
        seen = 0
        slot_err = None
        try:
            if mode == "bench":
                _run_bench_slot(log, writer, bench_slot_cap, bench_trial_cap,
                                cold_lock=scenario in COLD_LOCK_SLOTS)
            else:
                seen = _run_shadow_slot(log, writer, dur)
        except Exception as e:
            # A slot must NEVER take down the whole run: catch, log, and move
            # on to the next slot (the finally still flushes this slot's data).
            # This is what was missing when K1 threw -- it lost K2 + the tail.
            slot_err = e
            log.line("[instr] slot %s raised an exception (continuing):" % scenario)
            try:
                sys.print_exception(e)
            except Exception:
                pass
        finally:
            # Final drain BEFORE stop: rows, captures, and counters live on
            # the consumer and are gone once stop() destroys it.
            try:
                for _ in range(20):
                    writer.drain_csv()
                    writer.drain_captures(max_per_tick=2)
                log.mirror_stats()
                c = cam.instr_counters()
                writer.finish()
            except Exception as e2:
                sys.print_exception(e2)
                c = None
            cam.stop()
            gc.collect()
        # Automatic bad-slot flag for the unattended zero-touch block: a slot
        # that dispatched decoders but produced ~no unique fragments almost
        # certainly had a bad setup (out of frame, defocused, mount slipped).
        # It is loud in run.log so a post-run scan reveals exactly which slots
        # to redo -- without breaking the walk-away flow.
        if slot_err is not None:
            log.line("[instr] slot %s HEALTH: ERROR %r -> !! FAILED (partial data) !!"
                     % (scenario, slot_err))
        elif mode != "bench":
            dr = c.decode_rows if c else 0
            fx = c.frag_expired if c else 0
            if scenario in STATIC_SLOTS:
                # Static media: exactly 1 unique fragment is the HEALTHY
                # outcome (dedup collapses an unchanging QR), so judge
                # "decoded at all" instead of fragment throughput.
                verdict = ("OK (static media)" if seen >= 1
                           else "!! LIKELY-BAD (static QR never decoded) !!")
            else:
                verdict = "OK" if seen >= 3 else "!! LIKELY-BAD (redo?) !!"
            log.line("[instr] slot %s HEALTH: %d unique fragments, "
                     "%d dispatches, %d frags expired -> %s"
                     % (scenario, seen, dr, fx, verdict))
        log.line("[instr] slot %s done -> %s/ (%d frames captured)"
                 % (scenario, writer.scenario_dir, writer.n_frames))

    log.line("=== RUN %s COMPLETE (%d slots) ===" % (run_id, len(slots)))
    log.close()
    print("[instr] RUN %s complete; data in %s" % (run_id, run_dir))
    return run_dir


if __name__ == "__main__":
    main()
