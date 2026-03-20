# micropython_mods

Portable customization bundle for applying SeedSigner-related changes onto a fresh upstream MicroPython checkout.

## Expected layout under builder repo

Dependencies live under `deps/`:

- `<builder>/deps/micropython/upstream` — ephemeral MicroPython checkout
- `<builder>/deps/micropython/mods/` — this directory (patches + new files)
- `<builder>/deps/seedsigner-c-modules` — git submodule

Where `<builder>` is this repository root (`seedsigner-micropython-builder`).

## Layout

- `new_files/` — files that do not exist upstream and should be copied into MicroPython.
- `patches/` — modifications to existing upstream files.
- `BASELINE` — upstream/baseline metadata.

## Apply flow

```bash
# Uses <builder>/deps by default
scripts/verify_micropython_base.sh
scripts/apply_micropython_mods.sh

# Or pass a custom deps dir explicitly
scripts/verify_micropython_base.sh /path/to/deps
scripts/apply_micropython_mods.sh /path/to/deps
```

The apply script:
1. verifies workspace layout and MicroPython baseline,
2. overlays `new_files/`,
3. applies patches with `git apply --3way --index`,
4. writes `<deps>/micropython/upstream/.seedsigner-builder.env` with custom modules path.
