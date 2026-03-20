# seedsigner-micropython-builder

Build orchestration repo for SeedSigner MicroPython firmware without maintaining long-lived forks.

## Repository roles

- `deps/micropython/mods/` — overlay + patch set for MicroPython
- `deps/esp-idf/mods/` — overlay + patch set for ESP-IDF (placeholder)
- `ports/esp32/` — ESP32-specific hardware components (display, camera, power, BSP)
- `bindings/` — MicroPython C module bindings
- `scripts/` — shared setup/apply/build scripts used by both CI and local dev
- `.github/workflows/` — CI workflow that runs the same scripts

## Cloning and submodule setup

```bash
git clone https://github.com/kdmukAI-bot/seedsigner-micropython-builder.git
cd seedsigner-micropython-builder
git submodule update --init --recursive
```

The `--recursive` flag is required because `seedsigner-c-modules` contains its own
submodule (LVGL).

If you've already cloned without `--recursive`, run
`git submodule update --init --recursive` from the repo root.

## How `deps/` dependencies are managed

The `deps/` directory holds the external source trees needed for the build. They are
managed differently because they play different roles.

### `deps/seedsigner-c-modules` — git submodule (version-pinned)

This is the project's shared C module code (LVGL screens, navigation). It
is tracked as a **git submodule** so the builder repo records exactly which commit is
known-good. The build uses it as-is — no patches are applied.

- **Local:** populated by `git submodule update --init --recursive`
- **CI:** populated by `actions/checkout` with `submodules: true`
- **Version pin:** the submodule pointer in this repo is the single source of truth
- **Override:** `workflow_dispatch` accepts an optional `c_modules_ref` input to test a
  different branch/tag/SHA without changing the pin

### `deps/micropython/upstream` — ephemeral build workspace (not a submodule)

MicroPython is an upstream dependency that gets **patched and mutated** during every build.
The build starts from a clean upstream snapshot, applies the patch series from
`deps/micropython/mods/patches/`, overlays new files (board definitions,
partition tables) from `deps/micropython/mods/new_files/`, and compiles the
result. The modified tree is disposable — it is not committed or version-tracked.

- **Local:** seeded from the prebaked Docker image (`/opt/bases/micropython`) on first run
  by `scripts/prepare_sources_from_image.sh`. If the directory already exists, it is left
  as-is.
- **CI:** same seeding script copies the baseline from the Docker image into `deps/`.
- **Version pin:** the MicroPython version is pinned by `MICROPYTHON_REF` in `Dockerfile.ghcr`
  and recorded in `deps/micropython/mods/BASELINE`.
- **Developer mode:** if the MicroPython tree has uncommitted changes (dirty working tree),
  `apply_micropython_mods.sh` skips repatching and uses your current tree. This lets you
  edit MicroPython source directly and iterate without the patch step overwriting your work.

### Why the difference?

| | `seedsigner-c-modules` | `micropython` |
|---|---|---|
| Ownership | Project-owned code | Upstream dependency |
| Modified during build? | No — used as-is | Yes — patched + overlaid |
| Version tracking | Submodule commit pointer | Docker image `MICROPYTHON_REF` + `BASELINE` file |
| Working tree after build | Clean | Dirty (patched) |

A submodule implies a stable, tracked pointer — which makes sense for c-modules but not for
a tree that is immediately mutated. MicroPython's version is pinned in the Docker base image
and the `BASELINE` file instead.

### ESP-IDF

ESP-IDF is provided by the prebaked Docker image at `/opt/toolchains/esp-idf`. It is not
stored in `deps/` and is not a submodule.

Policy: local development is containerized only. Do not install build toolchains on host
machines.

## Local build (Docker only, GHCR base image)

```bash
make docker-shell
# inside container:
./scripts/setup_env.sh
./scripts/ci_build.sh
```

Optional override:

```bash
make docker-shell IMAGE=ghcr.io/<owner>/seedsigner-micropython-builder-base:latest
```

## One-liner Docker build (setup + firmware + screenshots)

```bash
make docker-build-all
```

Notes on default behavior:
- First run: if `deps/micropython/upstream` is missing, it is seeded from the Docker image automatically.
- Subsequent runs: if `deps/micropython/upstream` is dirty, the build uses your current working tree and skips repatching.

## CI

GitHub Actions workflow: `.github/workflows/build-firmware.yml`

CI checks out this repo with `submodules: true` (populating `seedsigner-c-modules` at its
pinned commit), seeds `deps/micropython/upstream` from the prebaked Docker image, applies mods,
builds firmware, and uploads artifacts.


## Build outputs

- Firmware build directory (default): `build/<BOARD>/`
- Build logs (default): `logs/`

These locations are rooted at the `seedsigner-micropython-builder` project directory.


## Screenshot generator

Run screenshot generator build + render:

```bash
./scripts/run_screenshot_generator.sh
# or with explicit deps dir
./scripts/run_screenshot_generator.sh /path/to/deps
```

Logs are written under `logs/` with timestamp-first naming.


## Flashing firmware

After a successful build, artifacts are under:

- `build/WAVESHARE_ESP32_S3_TOUCH_LCD_35B/`

Key files:

- `micropython.bin` (application image)
- `bootloader/bootloader.bin`
- `partition_table/partition-table.bin`
- `flash_args` (generated flash manifest)

Recommended flash command (uses generated offsets/files):

```bash
cd build/WAVESHARE_ESP32_S3_TOUCH_LCD_35B
python -m esptool --chip esp32s3 -b 460800   --before default_reset --after hard_reset   write_flash "@flash_args"
```

If needed, specify serial port explicitly:

```bash
python -m esptool --chip esp32s3 --port /dev/ttyACM0 -b 460800   --before default_reset --after hard_reset   write_flash "@flash_args"
```

Or with `idf.py` from MicroPython ESP32 port directory:

```bash
idf.py -p /dev/ttyACM0 flash
```


## Manual GitHub Actions runs

The workflow supports `workflow_dispatch` with optional inputs:

- `builder_ref` — branch/tag/SHA of this repo to run (default: `main`)
- `c_modules_ref` — branch/tag/SHA of `seedsigner-c-modules` to use (default: blank,
  meaning the pinned submodule commit)

Use this to test feature branches of either repo without changing default CI behavior.
Leaving `c_modules_ref` blank builds with whatever commit the submodule points at.

## Prebuilt base image (GHCR)

To reduce repeated setup work, CI uses a prebaked base image with:

- pinned MicroPython baseline (including submodules + prebuilt mpy-cross)
- pinned ESP-IDF baseline (tools installed)
- common host/build dependencies

Build/publish this image via workflow:

- `.github/workflows/build-base-image.yml`

Firmware workflow then seeds `deps/` from that image and applies project mods before build.


## ESP-IDF handling (current)

- Default path uses the prebaked ESP-IDF baseline from the builder image.
- `deps/esp-idf/mods` is currently a no-op placeholder (no IDF patch overlay applied).
- Optional future override is supported via `IDF_OVERRIDE_DIR=/path/to/esp-idf`.
