#!/usr/bin/env python3
"""Analyze instrumentation-run data captured by tools/instrumented_run.py.

Usage:
    python3 tools/analyze_instr_run.py <run_dir> [<run_dir> ...]

Each <run_dir> is a directory like data3/instr_43_r3 containing header.json,
run.log, and per-scenario subdirs with decode.csv / gate.csv / counters.json
(+ bench.json for K slots).

Produces a per-slot summary table plus the cross-slot analyses from
docs/instrumentation-run-spec.md section 6 (Q1-Q16).
"""

import csv
import json
import math
import os
import statistics
import sys
from collections import Counter, defaultdict

MISS_SENTINEL = -99


def pct(n, d):
    return 100.0 * n / d if d else float("nan")


def quantile(vals, q):
    if not vals:
        return float("nan")
    s = sorted(vals)
    idx = min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))
    return s[idx]


def load_slot(run_dir, scenario):
    d = os.path.join(run_dir, scenario)
    out = {"scenario": scenario}
    dpath = os.path.join(d, "decode.csv")
    gpath = os.path.join(d, "gate.csv")
    out["decode"] = list(csv.DictReader(open(dpath))) if os.path.exists(dpath) else []
    out["gate"] = list(csv.DictReader(open(gpath))) if os.path.exists(gpath) else []
    for name in ("counters", "bench"):
        p = os.path.join(d, name + ".json")
        out[name] = json.load(open(p)) if os.path.exists(p) else None
    fdir = os.path.join(d, "frames")
    out["frames"] = sorted(os.listdir(fdir)) if os.path.isdir(fdir) else []
    for r in out["decode"]:
        for k in ("seq", "ts_us", "decoder_id", "frame_gen", "content_seq", "passes",
                  "max_capstones", "seed_offset", "locked_offset", "win_offset",
                  "used_local", "blend_score", "bailed_blend", "bailed_noqr",
                  "dispatch_age_ms", "peer_busy", "peer_depth", "side_px", "luma",
                  "dark_lobe_peak"):
            r[k] = int(r[k])
        r["decode_ms"] = float(r["decode_ms"])
        r["sharpness"] = float(r["sharpness"])
    for r in out["gate"]:
        for k in ("ts_us", "frame_gen", "content_seq", "hamming_vs_last",
                  "gap_gens", "stable_ms", "dark_lobe_peak"):
            r[k] = int(r[k])
    return out


def slot_summary(slot):
    rows = slot["decode"]
    if not rows:
        return None
    dur = (rows[-1]["ts_us"] - rows[0]["ts_us"]) / 1e6
    n = len(rows)
    oc = Counter(r["outcome"] for r in rows)
    dec = oc.get("DECODED", 0)
    mis = oc.get("MISS", 0)
    located = dec + mis
    decoded_rows = [r for r in rows if r["outcome"] == "DECODED"]
    # unique fragments that decoded at least once
    frag_decoded = {r["content_seq"] for r in decoded_rows}
    new_per_s = len(frag_decoded) / dur if dur else float("nan")
    passes = [r["passes"] for r in rows]
    pass1 = sum(1 for r in decoded_rows if r["passes"] == 1)
    blend_dec = [r["blend_score"] for r in decoded_rows if r["blend_score"] >= 0]
    blend_mis = [r["blend_score"] for r in rows
                 if r["outcome"] == "MISS" and r["blend_score"] >= 0]
    dup_frags = Counter(r["content_seq"] for r in rows)
    dups = sum(1 for c in dup_frags.values() if c > 1)
    counters = slot["counters"] or {}
    return {
        "scenario": slot["scenario"],
        "dur": dur, "n": n, "dec": dec, "mis": mis,
        "nothing": oc.get("NOTHING", 0),
        "id_rate": pct(located, n),
        "ok_rate": pct(dec, located),
        "miss_rate": pct(mis, located),
        "new_per_s": new_per_s,
        "frag_decoded": len(frag_decoded),
        "frag_arrived": counters.get("frag_arrived"),
        "frag_expired": counters.get("frag_expired"),
        "passes_per_f": statistics.mean(passes) if passes else float("nan"),
        "pass1_rate": pct(pass1, dec),
        "blend_dec_p50": quantile(blend_dec, 0.5),
        "blend_dec_p95": quantile(blend_dec, 0.95),
        "blend_dec_max": max(blend_dec) if blend_dec else float("nan"),
        "blend_mis_p95": quantile(blend_mis, 0.95),
        "bail_blend": sum(r["bailed_blend"] for r in rows),
        "bail_noqr": sum(r["bailed_noqr"] for r in rows),
        "decode_ms_p50": quantile([r["decode_ms"] for r in rows], 0.5),
        "age_p50": quantile([r["dispatch_age_ms"] for r in rows], 0.5),
        "age_p95": quantile([r["dispatch_age_ms"] for r in rows], 0.95),
        "peer_busy_rate": pct(sum(r["peer_busy"] for r in rows), n),
        "dup_frags": dups,
        "multi_dispatch_frac": pct(dups, len(dup_frags)),
        "frames": len(slot["frames"]),
    }


def print_summary_table(name, summaries):
    cols = ["scenario", "dur", "n", "dec", "mis", "nothing", "id_rate", "ok_rate",
            "new_per_s", "passes_per_f", "pass1_rate", "blend_dec_p50",
            "blend_dec_p95", "blend_dec_max", "bail_blend", "bail_noqr",
            "decode_ms_p50", "age_p50", "peer_busy_rate", "dup_frags",
            "frag_expired", "frames"]
    print(f"\n### {name}: per-slot summary")
    print(" | ".join(cols))
    for s in summaries:
        if s is None:
            continue
        vals = []
        for c in cols:
            v = s[c]
            if isinstance(v, float):
                vals.append(f"{v:.1f}" if not math.isnan(v) else "-")
            else:
                vals.append(str(v))
        print(" | ".join(vals))


def lock_analysis(slot):
    """Q14: lock trajectories, pass-1 conditioned on lock stability, run lengths."""
    rows = slot["decode"]
    out = {}
    for dec_id in (0, 1):
        drows = [r for r in rows if r["decoder_id"] == dec_id]
        if not drows:
            continue
        locks = [r["locked_offset"] for r in drows]
        moves = sum(1 for a, b in zip(locks, locks[1:]) if a != b)
        decoded = [r for r in drows if r["outcome"] == "DECODED"]
        # pass-1 rate conditioned on lock unchanged since previous row
        p1_stable = p1_moved = n_stable = n_moved = 0
        prev_lock = None
        for r in drows:
            if prev_lock is not None and r["outcome"] == "DECODED":
                if r["locked_offset"] == prev_lock:
                    n_stable += 1
                    p1_stable += r["passes"] == 1
                else:
                    n_moved += 1
                    p1_moved += r["passes"] == 1
            prev_lock = r["locked_offset"]
        out[dec_id] = {
            "rows": len(drows), "decoded": len(decoded), "lock_moves": moves,
            "lock_values": Counter(locks),
            "p1_lock_stable": pct(p1_stable, n_stable), "n_stable": n_stable,
            "p1_lock_moved": pct(p1_moved, n_moved), "n_moved": n_moved,
        }
    # win_offset run lengths across DECODED rows (serial)
    wins = [r["win_offset"] for r in rows if r["outcome"] == "DECODED"]
    runs = []
    if wins:
        cur = 1
        for a, b in zip(wins, wins[1:]):
            if a == b:
                cur += 1
            else:
                runs.append(cur)
                cur = 1
        runs.append(cur)
    out["win_run_median"] = statistics.median(runs) if runs else float("nan")
    out["win_run_max"] = max(runs) if runs else 0
    out["win_hist"] = Counter(wins)
    return out


def dup_analysis(slot):
    """Q15: duplicated dispatches per fragment; value of the accidental retry."""
    rows = slot["decode"]
    byfrag = defaultdict(list)
    for r in rows:
        byfrag[r["content_seq"]].append(r)
    dup_frags = {k: v for k, v in byfrag.items() if len(v) > 1}
    n_dup_dispatch = sum(len(v) - 1 for v in dup_frags.values())
    # overlapping double-dispatch: same frag on both decoders, time windows overlap
    overlap = 0
    retry_win = 0   # 2nd+ dispatch DECODED after an earlier MISS of same frag
    retry_seen = 0
    for frag, v in dup_frags.items():
        v = sorted(v, key=lambda r: r["ts_us"])
        decs = {r["decoder_id"] for r in v}
        if len(decs) > 1:
            for a, b in zip(v, v[1:]):
                if a["decoder_id"] != b["decoder_id"] and \
                        b["ts_us"] < a["ts_us"] + a["decode_ms"] * 1000:
                    overlap += 1
        missed_first = False
        for r in v:
            if r["outcome"] == "MISS":
                missed_first = True
            elif r["outcome"] == "DECODED" and missed_first:
                retry_seen += 1
                retry_win += 1
                break
    return {
        "frags": len(byfrag), "dup_frags": len(dup_frags),
        "dup_dispatches": n_dup_dispatch,
        "overlapping_double": overlap,
        "retry_rescued": retry_win,
    }


def gate_analysis(slot):
    """Q2: hamming/gap/stability distributions + decision mix."""
    rows = slot["gate"]
    if not rows:
        return None
    ham = [r["hamming_vs_last"] for r in rows if r["hamming_vs_last"] >= 0]
    gaps = [r["gap_gens"] for r in rows]
    dec = Counter(r["decision"] for r in rows)
    return {
        "rows": len(rows),
        "ham_p50": quantile(ham, 0.5), "ham_p95": quantile(ham, 0.95),
        "ham_min": min(ham) if ham else float("nan"),
        "low_ham_frac": pct(sum(1 for h in ham if h <= 60), len(ham)),
        "gap_p50": quantile(gaps, 0.5), "gap_p95": quantile(gaps, 0.95),
        "decisions": dict(dec),
    }


def ae_transient(slot, window_s=10.0):
    """Q6: dark_lobe_peak over the first window_s of the slot (gate rows,
    falling back to decode rows for gate-off slots)."""
    rows = slot["gate"] or slot["decode"]
    if not rows:
        return None
    t0 = rows[0]["ts_us"]
    win = [(r["ts_us"] - t0) / 1e6 for r in rows]
    lobe = [r["dark_lobe_peak"] for r in rows]
    first = [(t, l) for t, l in zip(win, lobe) if t <= window_s]
    rest = [l for t, l in zip(win, lobe) if t > window_s]
    if not first:
        return None
    fvals = [l for _, l in first]
    settled = statistics.median(rest) if rest else statistics.median(fvals)
    # direction changes in the smoothed first-10s trace = ringing indicator
    changes = 0
    prev_dir = 0
    for a, b in zip(fvals, fvals[1:]):
        d = (b > a) - (b < a)
        if d and prev_dir and d != prev_dir:
            changes += 1
        if d:
            prev_dir = d
    return {
        "start": fvals[0], "min10": min(fvals), "max10": max(fvals),
        "settled": settled, "reversals10": changes,
        "overshoot": max(fvals) - settled,
    }


def capstone_analysis(slot, cap):
    """Q5: max_capstones on rows that burned the full cap and missed."""
    rows = [r for r in slot["decode"]
            if r["outcome"] != "DECODED" and r["passes"] >= cap]
    return Counter(r["max_capstones"] for r in rows), len(rows)


def parse_parts_log(path):
    """Read a bench slot's ur_parts.log (every payload fed to the decoder) and
    return (fmt, parts_per_cycle, n_lines). The same log serves UR fountains and
    BBQr cycles -- the format is detected from the payload header:
      BBQr:  B$ <enc> <ftype> <total b36 x2> <index b36 x2> <data>  -> cyclic;
             parts_per_cycle = total (fixed).
      UR:    UR:TYPE/<seq>-<len>/<bytewords>  -> rateless fountain; `len` is the
             pure-fragment count (a floor on unique fragments, NOT a cycle)."""
    if not os.path.exists(path):
        return ("?", None, 0)
    totals, n, fmt = set(), 0, "?"
    with open(path, "rb") as f:
        for raw in f:
            s = raw.strip().decode("latin-1", "replace")
            if not s:
                continue
            n += 1
            up = s.upper()
            if up.startswith("B$") and len(s) >= 8:
                fmt = "BBQR"
                try:
                    totals.add(int(s[4:6], 36))
                except ValueError:
                    pass
            elif up.startswith("UR:"):
                fmt = "UR"
                try:
                    totals.add(int(s.split("/")[1].split("-")[1]))
                except (IndexError, ValueError):
                    pass
    return (fmt, (max(totals) if totals else None), n)


def bench_report(run_dir, slots, header):
    """Completion-benchmark report over every mode==bench slot (K1/K2, the BBQr
    trio BSTK/BBASE/BGATE, D-profile K1/KD, ...). median trial_ms is the headline
    (for the BBQr bench it is cold-start-inclusive by construction -- each trial
    resets the lock). Adds BBQr cycle economics: parts/cycle, cycles-to-complete,
    and the located-miss rate that the cyclic penalty multiplies."""
    fps = header.get("sparrow_fps")
    cfg_of = {s["scenario"]: s.get("config", "?") for s in header["slots"]}
    bench_scen = [s["scenario"] for s in header["slots"] if s.get("mode") == "bench"]
    meds = {}
    print("\n### Completion benchmark (median time-to-complete)")
    for scen in bench_scen:
        slot = slots.get(scen)
        if not slot or not slot.get("bench"):
            continue
        b = slot["bench"]
        trials = b.get("trial_ms", [])
        med = statistics.median(trials) if trials else float("nan")
        meds[scen] = med if trials else None
        fmt, ppc, nparts = parse_parts_log(os.path.join(run_dir, scen, "ur_parts.log"))
        summ = slot_summary(slot)
        miss = summ["miss_rate"] if summ else float("nan")
        p1 = summ["pass1_rate"] if summ else float("nan")
        extra = ""
        if fmt == "BBQR" and ppc:
            extra += f" parts/cycle={ppc}"
            if fps and trials:
                cyc_ms = ppc / fps * 1000.0
                extra += f" cycle={cyc_ms/1000:.1f}s@{fps}fps cycles~{med/cyc_ms:.1f}"
        cold = b.get("cold_lock")
        print(f"{scen} [{cfg_of.get(scen,'?')}] fmt={fmt}"
              f"{' cold-lock' if cold else ''}: "
              f"median={med:.0f}ms trials={trials} completions={b.get('completions')} "
              f"resets={b.get('resets')} miss={miss:.0f}% pass1={p1:.0f}% "
              f"parts_fed={b.get('ur_parts_fed')} cur_err={b.get('cur_errors')}{extra}")
    # Headline A/Bs: stock->PR#11 (BSTK->BBASE) and the blend-gate delta
    # (BBASE->BGATE). Lower time is better; ratio < 1 = faster.
    def _ratio(a, b, label):
        if meds.get(a) and meds.get(b):
            print(f"   {label}: {meds[b]:.0f}/{meds[a]:.0f} = {meds[b]/meds[a]:.2f}x")
    if "BSTK" in meds or "K1" in meds:
        print("   -- ratios (of median time-to-complete; <1 = faster) --")
        _ratio("BSTK", "BBASE", "PR#11 vs stock   (BBASE/BSTK)")
        _ratio("BBASE", "BGATE", "blend gate delta (BGATE/BBASE)")
        _ratio("K1", "K2", "full stack vs base (K2/K1)")


def main(run_dirs):
    for run_dir in run_dirs:
        header = json.load(open(os.path.join(run_dir, "header.json")))
        scen_order = [s["scenario"] for s in header["slots"]]
        slots = {s: load_slot(run_dir, s) for s in scen_order
                 if os.path.isdir(os.path.join(run_dir, s))}
        print(f"\n\n## RUN {header['run_id']}  ({run_dir})")
        print(f"profile={header['profile']} sparrow_fps(header)={header['sparrow_fps']}")
        sums = [slot_summary(slots[s]) for s in scen_order if s in slots]
        print_summary_table(header["run_id"], sums)

        bench_report(run_dir, slots, header)

        print("\n### Q14 lock analysis (A1 vs A8)")
        for s in ("A1", "A5", "A8"):
            if s not in slots:
                continue
            la = lock_analysis(slots[s])
            print(f"{s}: win_run median={la['win_run_median']} max={la['win_run_max']} "
                  f"win_hist={dict(sorted(la['win_hist'].items()))}")
            for did in (0, 1):
                if did in la:
                    d = la[did]
                    print(f"   dec{did}: rows={d['rows']} decoded={d['decoded']} "
                          f"lock_moves={d['lock_moves']} "
                          f"locks={dict(sorted(d['lock_values'].items()))} "
                          f"p1|stable={d['p1_lock_stable']:.0f}%(n={d['n_stable']}) "
                          f"p1|moved={d['p1_lock_moved']:.0f}%(n={d['n_moved']})")

        print("\n### Q15 duplicate-dispatch analysis")
        for s in ("A1", "A2", "A5", "A6", "A7"):
            if s not in slots:
                continue
            da = dup_analysis(slots[s])
            print(f"{s}: frags={da['frags']} dup_frags={da['dup_frags']} "
                  f"extra_dispatches={da['dup_dispatches']} "
                  f"overlapping={da['overlapping_double']} "
                  f"retry_rescued={da['retry_rescued']}")

        print("\n### Q2 gate/hamming analysis")
        for s in scen_order:
            if s not in slots:
                continue
            ga = gate_analysis(slots[s])
            if ga:
                print(f"{s}: rows={ga['rows']} ham p50/p95/min="
                      f"{ga['ham_p50']}/{ga['ham_p95']}/{ga['ham_min']} "
                      f"<=60:{ga['low_ham_frac']:.0f}% gap p50/p95="
                      f"{ga['gap_p50']}/{ga['gap_p95']} dec={ga['decisions']}")

        print("\n### Q6 AE transient (first 10 s per slot)")
        for s in scen_order:
            if s not in slots:
                continue
            ae = ae_transient(slots[s])
            if ae:
                print(f"{s}: start={ae['start']} min={ae['min10']} max={ae['max10']} "
                      f"settled={ae['settled']:.0f} reversals={ae['reversals10']} "
                      f"overshoot={ae['overshoot']:.0f}")

        print("\n### Q5 capstones on full-cap missed rows")
        for s, cap in (("A1", 4), ("A3", 3), ("A5", 4)):
            if s not in slots:
                continue
            hist, n = capstone_analysis(slots[s], cap)
            print(f"{s} (cap={cap}): n={n} capstones={dict(sorted(hist.items()))}")


if __name__ == "__main__":
    main(sys.argv[1:])
