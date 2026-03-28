# micropython_mods

Portable customization bundle for applying SeedSigner-related changes onto a fresh upstream MicroPython checkout.

## Expected layout under builder repo

Dependencies live under `deps/`:

- `<builder>/deps/micropython/upstream` — MicroPython git submodule (pinned to v1.27.0)
- `<builder>/deps/micropython/mods/` — this directory (patches + new files)
- `<builder>/deps/seedsigner-c-modules` — git submodule

Where `<builder>` is the repository root (`seedsigner-micropython-builder`).

## Layout

- `new_files/` — files that do not exist upstream and should be copied into MicroPython (board definitions, partition tables).
- `patches/` — modifications to existing upstream files (build system, cmake, sdkconfig).
- `BASELINE` — upstream baseline metadata (ref, IDF version).

## Workflow

### Apply patches (build time)

```bash
scripts/apply_micropython_mods.sh
```

This is called automatically during the build. It:
1. Verifies the submodule is at the clean pinned commit
2. Applies patches with `git apply`
3. Copies new_files overlay into the tree
4. Commits the result so developer edits are cleanly separated

### Restore to clean state

```bash
scripts/restore_micropython_clean.sh
```

Resets the MicroPython submodule to its clean pinned commit, discarding all patches and local edits. Run this before re-applying patches or when the tree is in an unknown state.

### Edit and regenerate patches

```bash
# 1. Start from a patched tree (or apply patches first)
scripts/apply_micropython_mods.sh

# 2. Make your changes to files under deps/micropython/upstream/ports/
#    (edit cmake files, sdkconfig, etc.)

# 3. Regenerate the patch
scripts/generate_micropython_patch.sh

# 4. Verify: restore clean, re-apply, build
scripts/restore_micropython_clean.sh
scripts/apply_micropython_mods.sh
```

The generate script:
- Diffs the current tree against the clean pinned commit
- Only captures changes under `ports/` (excludes `lib/` submodule noise)
- Excludes files managed by `new_files/` overlay
- Verifies the generated patch applies cleanly without `--3way`

### Adding new files

Files that don't exist in upstream MicroPython (board definitions, custom partition tables) go in `new_files/`, mirroring the MicroPython directory structure:

```
new_files/
  ports/esp32/
    boards/WAVESHARE_ESP32_S3_TOUCH_LCD_35B/
      mpconfigboard.h
      mpconfigboard.cmake
      sdkconfig.defaults
    partitions-16MiB-waveshare.csv
```

These are copied verbatim by `apply_micropython_mods.sh` and are **not** included in the patch file.
