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

The `--recursive` flag is required because `seedsigner-lvgl-screens` contains its own
submodule (LVGL).

If you've already cloned without `--recursive`, run
`git submodule update --init --recursive` from the repo root.

## How `deps/` dependencies are managed

The `deps/` directory holds the external source trees needed for the build. They are
managed differently because they play different roles.

### `deps/seedsigner-lvgl-screens` — git submodule (version-pinned)

This is the project's shared C module code (LVGL screens, navigation). It
is tracked as a **git submodule** so the builder repo records exactly which commit is
known-good. The build uses it as-is — no patches are applied.

- **Local:** populated by `git submodule update --init --recursive`
- **CI:** populated by `actions/checkout` with `submodules: true`
- **Version pin:** the submodule pointer in this repo is the single source of truth
- **Override:** `workflow_dispatch` accepts an optional `screens_ref` input to test a
  different branch/tag/SHA without changing the pin

### `deps/micropython/upstream` — patched build workspace (a pinned submodule)

MicroPython is a pinned git submodule that gets **patched and mutated** during every build.
The build starts from the clean pinned snapshot, applies the patch series from
`deps/micropython/mods/patches/`, overlays new files (board definitions,
partition tables) from `deps/micropython/mods/new_files/`, and compiles the
result. The patch + overlay are committed inside the submodule as a single
`seedsigner-builder: applied patch …` commit (nested `lib/` submodules are
excluded from it). A clean `make docker-build-all` restores the submodule to its
pinned commit on exit — preserving `lib/` submodules — so the submodule pointer,
not the mutated tree, is the source of truth.

- **Local:** populated by `git submodule update --init --recursive`.
- **CI:** populated by `actions/checkout` with `submodules: true`.
- **Version pin:** the submodule pointer (recorded in `deps/micropython/mods/BASELINE`) is
  the single source of truth.
- **Developer mode:** if the MicroPython tree has uncommitted changes (dirty working tree),
  `apply_micropython_mods.sh` skips repatching and uses your current tree. This lets you
  edit MicroPython source directly and iterate without the patch step overwriting your work.

### Why the difference?

| | `seedsigner-lvgl-screens` | `micropython` |
|---|---|---|
| Ownership | Project-owned code | Upstream dependency |
| Modified during build? | No — used as-is | Yes — patched + overlaid |
| Version tracking | Submodule commit pointer | Submodule commit pointer + `BASELINE` file |
| Working tree after build | Clean | Patched during build, restored to pinned after a clean build |

A submodule implies a stable, tracked pointer — which makes sense for lvgl-screens but not for
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
- First run: initialize submodules (`git submodule update --init --recursive`) if `deps/micropython/upstream` is empty.
- Subsequent runs: if `deps/micropython/upstream` is dirty, the build uses your current working tree and skips repatching.

## CI

GitHub Actions workflow: `.github/workflows/build-firmware.yml`

CI checks out this repo with `submodules: true` (populating `seedsigner-lvgl-screens` and
`deps/micropython/upstream` at their pinned commits), applies mods, builds firmware, and
uploads artifacts.


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

- `build/<BOARD>/` (e.g. `build/WAVESHARE_ESP32_S3_TOUCH_LCD_35B/`)

Key files:

- `micropython.bin` (application image)
- `bootloader/bootloader.bin`
- `partition_table/partition-table.bin`
- `flash_args` (generated flash manifest)

Recommended flash command (uses generated offsets/files):

```bash
# ESP32-S3 boards
cd build/WAVESHARE_ESP32_S3_TOUCH_LCD_35B
python -m esptool --chip esp32s3 -b 460800   --before default_reset --after hard_reset   write_flash "@flash_args"

# ESP32-P4 boards
cd build/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43
python -m esptool --chip esp32p4 --port /dev/ttyACM0   write_flash "@flash_args"
```


## Manual GitHub Actions runs

The workflow supports `workflow_dispatch` with optional inputs:

- `builder_ref` — branch/tag/SHA of this repo to run (default: `main`)
- `screens_ref` — branch/tag/SHA of `seedsigner-lvgl-screens` to use (default: blank,
  meaning the pinned submodule commit)

Use this to test feature branches of either repo without changing default CI behavior.
Leaving `screens_ref` blank builds with whatever commit the submodule points at.

## Prebuilt base image

To reduce repeated setup work, builds run inside a prebaked base image with:

- pinned ESP-IDF baseline (tools installed)
- common host/build dependencies

MicroPython is **not** baked in — it comes from the `deps/micropython/upstream`
submodule (see above), so the image only changes on an ESP-IDF bump.

This image is built and published **manually** (not by a CI job) — see
[Rebuilding and publishing the base image](README-dev.md#rebuilding-and-publishing-the-base-image)
in the developer guide.

Firmware workflow then seeds `deps/` from that image and applies project mods before build.


## ESP-IDF handling (current)

- Default path uses the prebaked ESP-IDF baseline from the builder image.
- `deps/esp-idf/mods` is currently a no-op placeholder (no IDF patch overlay applied).
- Optional future override is supported via `IDF_OVERRIDE_DIR=/path/to/esp-idf`.
