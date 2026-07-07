# Language packs — deploy the app's payload; point ONLY at the app (finalized 2026-07-06)

**Status:** REWRITTEN 2026-07-06 to the finalized design. **This supersedes the earlier version**
(which had this repo resolve packs from `SS_LANGPACKS_REPO_DIR` — the pack-repo checkout — and
build-on-first-use). That is replaced by a single pointer at the **app**. Nothing here is executed
yet.

**Authoritative spec:** `seedsigner-lvgl-screens/docs/language-pack-production-and-delivery-notes.md`
(§3 deployers, §7 builder). This file is the builder-scoped execution brief.

---

## The model in one paragraph

The ESP32 firmware build deploys the **SeedSigner app**, and the app is a self-contained tree that
already **bundles its language packs at `src/lang-packs`**. So this repo needs exactly **one
pointer — `SS_APP_DIR`** — and stages `$SS_APP_DIR/src/lang-packs` onto the microSD. It does **not**
know about the pack repo, does **not** build packs, and does **not** branch on signed-vs-dev
(whatever the app bundled is what deploys). Empty/absent packs = a valid **English-only** deploy.

## What this repo does today

- `tools/sd_format_push.py` — pure byte-copy stager (runtime files only: `.ttf`, `runs.bin`,
  `manifest.json`, `endonym_*.bin`, `LC_MESSAGES/messages.mo`; skips `runs.json`). Its pack source
  resolves via `_devenv.resolve_packs()` → `SS_PACKS_DIR` | `SS_LANGPACKS_REPO_DIR/{build,signed-packs}`
  (default `$SS_APP_DIR/deps/language-packs`, with build-on-first-use).
- `tools/deploy_app.py` — `SS_SRC = $SS_APP_DIR/src/seedsigner` (already env-driven; correct).
- No `deps/language-packs` submodule (good — keep it that way).

## What to change

1. **Repoint the pack source to the app's staged payload.** In `_devenv.resolve_packs()` (and the
   `sd_format_push.py --packs` default), resolve to **`$SS_APP_DIR/src/lang-packs`**. Keep a plain
   `--packs DIR` / `SS_PACKS_DIR` override for oddball layouts, but the default is the app.
2. **Delete `SS_LANGPACKS_REPO_DIR`, `SS_LANGPACKS_USE_SIGNED`, the build-on-first-use block, and
   the pack-repo `.po`-submodule bootstrap.** This repo neither builds nor knows the pack repo.
   (The pack build is driven from the live pack-repo checkout into the app; see the spec's dev flow.)
3. **Absent/empty `src/lang-packs` = English-only deploy, not an error.** Stage whatever is there
   (including nothing); the app renders English from its baked floor.
4. **Keep** `sd_format_push.py`'s runtime-file filtering and `deploy_app.py`'s `SS_SRC`.
5. **Verify on device:** stage, boot, switch locale, check a CJK/Indic/RTL screen; and verify a
   **no-packs** flash boots English-only cleanly.

## Manifest migration (fast-track — this repo's part of retiring screens' `locales.h`)

Screens is moving per-locale font policy out of a baked table into each pack's `manifest.json`
(spec §3a). The ESP32 binding must **register each `/sd/<locale>/manifest.json` via
`ss_register_pack_manifest`** at pack discovery, so on-device rendering takes policy from the
manifest. (The `bindings/` are migrating into this repo per the ecosystem plan, so this lands
here.) Order across repos is free (it's all local dev — expect non-English broken until the whole
sweep, incl. screens deleting `locales.h`, is done).

## Notes / gotchas

- The `.env` contract shrinks to essentially **`SS_APP_DIR`** (+ firmware/port paths). No
  pack-repo vars.
- Pack `.mo` compile from the **kdmukAI-bot fork** of `seedsigner-translations` (ur stub) — the
  app already staged them; this repo just copies.
- **Out of scope:** SeedSigner-OS + the production MicroPython build (assemble their own pieces).

## References
- Screens spec: `seedsigner-lvgl-screens/docs/language-pack-production-and-delivery-notes.md`.
- Pack repo: `kdmukAI-bot/seedsigner-language-packs` (`README.md`).
- This repo: `docs/language-selection-integration-todo.md` (runtime picker side).
</content>
