# On-device `.mo` catalogs: the second i18n asset stream (Layer 1) the device doesn't ship yet

## Context
SeedSigner's LVGL i18n is two layers: *text translation* (msgid → translated
`msgstr`) and *rendering* (fonts + glyph shaping). On MicroPython the device today
ships **only the rendering half** — so localized screens render in **English**.
This note is the firmware/deploy half of closing that gap (spec Deliverable A).

Cross-repo overview: `seedsigner-lvgl-screens/docs/knowledge/i18n-text-vs-render-platform-asymmetry.md`.
Python reader / build half: `seedsigner/docs/knowledge/micropython-i18n-text-layer.md`.
Full design: `seedsigner-lvgl-screens/docs/i18n-ngettext-micropython-spec.md`.

## The key fact: there are TWO separate per-locale asset streams on the device
They are easy to conflate because both are per-locale and both live on the SD card,
but they are read by **different code on different sides of the C/Python boundary**:

| Stream | Files | Read by | Layer |
|---|---|---|---|
| **Font packs** | `<locale>/*.ttf` + `runs.bin` (+ `manifest.json`) | the **C** seam — `dm_load_locale()` → `ss_load_locale()` via the `ss_pack_provider_t` (`mp_pack_provider` reads SD into a dict the host pre-stages) | 2 (render) |
| **Translation catalogs** | `<locale>/LC_MESSAGES/messages.mo` | **Python** — the `compat/_mo` reader in the deployed app (NOT the C loader) | 1 (text) |

The font-pack stream is wired and validated. The `.mo` stream **does not exist on
device yet** — neither the catalogs are staged onto SD nor is the Python reader
present (it is unbuilt; see the seedsigner note). That is exactly why the device
runs English regardless of the selected locale.

## What this repo owns for Deliverable A
1. **Stage the `.mo` catalogs onto SD** alongside the font packs, per locale, at the
   `localedir` path the Python layer expects (`…/l10n/<locale>/LC_MESSAGES/messages.mo`).
   Whatever tooling deploys the font packs / app (`tools/deploy_app.py`, pack
   staging) must also place the `.mo` files. The Python reader is **app code**
   deployed with the rest of the app — not frozen firmware — so the catalogs are
   plain SD data assets next to it.
2. **Integrity before parse.** On a signing device the `.mo` must be verified
   (signature/hash) before the Python reader parses it, mirroring how the font packs
   are treated. The reader must *also* be defensively bounds-checked (treat the file
   as untrusted, fail closed to the English passthrough) — but the firmware/deploy
   side is responsible for the verify-before-use gate and for getting trusted bytes
   onto the partition.
3. **SD plumbing reuse.** The `.mo` stream rides the same `machine.SDCard` + `VfsFat`
   mount the font packs already use. Heads-up on the SD path itself:
   - the FATFS link collision resolved in
     [micropython-fatfs-vs-esp-idf-fatfs-collision.md](micropython-fatfs-vs-esp-idf-fatfs-collision.md)
   - P4 microSD power in [esp32-p4-sdcard-ldo-power.md](esp32-p4-sdcard-ldo-power.md)

## Why text is NOT delivered through the font-pack seam
Tempting shortcut, but wrong: the C font-pack loader only installs fonts + glyph
runs, and the glyph-run table is **keyed by the already-translated `msgstr`** — it
consumes translated text, it does not produce it. Plural-form selection also needs
`n` at the Python call site, which the C layer never sees. So the `.mo` catalogs
must be read **Python-side**, as their own SD asset stream, independent of
`dm_load_locale`. Keeping these two streams distinct (and content-version-matched —
a stale `runs.bin` against a newer `.mo` tofus) is the whole point.

## Status
- Font-pack stream (Layer 2): present + validated on ESP32-P4.
- `.mo` catalog stream (Layer 1): **not staged, not read** → device shows English.
  Lands when Deliverables A (this note + the seedsigner build half) and B (the
  reader) ship together.
</content>
