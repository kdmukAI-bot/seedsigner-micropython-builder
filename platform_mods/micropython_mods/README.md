# micropython_mods

Portable customization bundle for applying SeedSigner-related changes onto a fresh upstream MicroPython checkout.

## Expected layout under builder repo

Use a sources under this repo (default: `sources/`):

- `<builder>/sources/micropython`
- `<builder>/sources/seedsigner-c-modules`
- `<builder>/sources/seedsigner-micropython-builder` (optional mirror; this repo itself lives at `<builder>`)

Where `<builder>` is this repository root (`seedsigner-micropython-builder`).

## Layout

- `new_files/` — files that do not exist upstream and should be copied into MicroPython.
- `patches/` — modifications to existing upstream files.
- `BASELINE` — upstream/baseline metadata.

## Apply flow

```bash
# Uses <builder>/sources by default
scripts/verify_micropython_base.sh
scripts/apply_micropython_mods.sh

# Or pass a custom sources explicitly
scripts/verify_micropython_base.sh /path/to/sources
scripts/apply_micropython_mods.sh /path/to/sources
```

The apply script:
1. verifies workspace layout and MicroPython baseline,
2. overlays `new_files/`,
3. applies patches with `git apply --3way --index`,
4. writes `<sources>/micropython/.seedsigner-builder.env` with custom modules path.
