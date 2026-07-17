# k_quirc contrast-stretch: what it was, why it's not in mainline, where it lives

**Status (2026-07-17): removed from `seedsigner-dev`. The code exists ONLY on the
standalone experimental branch `feat/contrast-stretch` of `kdmukAI-bot/k_quirc`
(tip `385187c`). It is in no compile path of any firmware build.**

## The approach

A preprocessing pass in `k_quirc_identify.c`, run on the grayscale frame before
Otsu thresholding:

1. Build the 256-bin histogram of the frame.
2. Clip `K_QUIRC_CONTRAST_CLIP_PCT` percent of pixels from each tail
   (default 5%: darkest 5% and brightest 5%).
3. Linearly remap the surviving range to 0–255 via a LUT (one pass over the
   image).
4. Early-out: skip the remap entirely when the resulting stretch would be
   < ~15% (integer math; commit `385187c` replaced an earlier hardcoded
   `range >= 230` check).

Gated behind `CONFIG_K_QUIRC_CONTRAST_STRETCH` (ESP-IDF Kconfig) /
`K_QUIRC_CONTRAST_STRETCH` (compile definition). Cost: one histogram pass + one
LUT remap per frame.

**Hypothesis it was built on:** low-contrast frames (dim lighting, washed-out
paper QRs) defeat global Otsu thresholding; stretching the histogram first
should recover them. Suspected value was paper QRs under poor lighting — never
the LCD-scan population that dominates SeedSigner use.

## Why it was removed from mainline

- **The A/B measured it unhelpful-to-harmful on our actual workload:** mildly
  harmful on the P4-43, neutral on the P4-35
  (`docs/k_quirc-adaptive-threshold-build-results.md`), and the offline
  miss-corpus results were identical with it off
  (`docs/k_quirc-throughput-lever-findings.md` corpora are contrast-stretch
  OFF).
- **The real root cause of QR misses was elsewhere:** a fixed +10 threshold
  offset wrong for LCD-displayed QRs, fixed properly by the adaptive
  bootstrap-sweep + lock (see
  `docs/knowledge/qr-decode-miss-threshold-root-cause.md`). Contrast-stretch
  was an earlier guess at the same symptom.

## The config-contamination incident (why this doc exists)

The Kconfig entry shipped with `default y`, and both P4 boards' committed
`sdkconfig.board` files carried an explicit `CONFIG_K_QUIRC_CONTRAST_STRETCH=y`
— while the *documented* decision from the A/B was OFF (the `=n` was only ever
set in local A/B builds, never committed). Result: essentially every firmware
built during the 2026-07 instrumentation/bench campaigns (including the BBQr
cycle bench, data12–data14) silently ran contrast-stretch, contradicting the
recorded experimental conditions and invalidating cross-build magnitude
comparisons.

Lesson: an experimental preprocessing pass must never sit below keeper commits
with a `default y` — an unproven feature defaults OFF, and better, stays out of
the branch entirely until proven.

## Where the code lives / how to resurrect it

- Branch: `feat/contrast-stretch` on `kdmukAI-bot/k_quirc`, tip `385187c` =
  infra keeper `d943226` + `a35df85` (preprocessing + Kconfig) + `385187c`
  (stretch-% early-out). The branch shares `d943226` with `seedsigner-dev`
  history, so it rebases forward cleanly.
- Removal from `seedsigner-dev` was a rebase-drop
  (`git rebase --onto d943226 385187c`), 2026-07-17; the five adaptive-threshold
  commits replayed conflict-free (old `a6ed873..7293ee1` → new
  `630fc94..d80269a`), proving there was no structural dependency.
- To experiment again: rebase `feat/contrast-stretch` onto the current
  `seedsigner-dev`, build with the Kconfig explicitly enabled, and change the
  `default y` to `default n` first.
