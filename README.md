# seedsigner-micropython-builder

Build orchestration repo for SeedSigner MicroPython firmware without maintaining long-lived forks.

## Repository roles

- `platform_mods/micropython_mods/` — overlay + patch set for MicroPython
- `platform_mods/idf_mods/` — overlay + patch set for ESP-IDF
- `scripts/` — shared setup/apply/build scripts used by both CI and local dev
- `.github/workflows/` — CI workflow that runs the same scripts

## Default local sources layout

By default scripts assume sources are under `sources/` in this repo:

- `sources/micropython`
- `sources/seedsigner-c-modules`
- `sources/esp-idf`

## Local build (host)

```bash
./scripts/setup_env.sh
./scripts/ci_build.sh
```

## Local build (Docker)

```bash
make docker-build
make docker-shell
# inside container:
./scripts/setup_env.sh
./scripts/ci_build.sh
```

## CI

GitHub Actions workflow: `.github/workflows/build-firmware.yml`

It clones required upstream repos into `sources/`, applies mods, builds firmware, and uploads artifacts.

### ESP-IDF tools path (`IDF_TOOLS_PATH`)

Build scripts default ESP-IDF tools to a workspace-local path for compatibility across environments:

- preferred: `<repo>/.espressif`
- fallback (if workspace is not writable): `$HOME/.espressif`

In GitHub Actions, the firmware workflow sets:

- `IDF_TOOLS_PATH=${{ github.workspace }}/.espressif`

You can override `IDF_TOOLS_PATH` explicitly in both CI and local Docker runs if needed.


## Build outputs

- Firmware build directory (default): `build/<BOARD>/`
- Build logs (default): `logs/`

These locations are rooted at the `seedsigner-micropython-builder` project directory.


## Screenshot generator

Run screenshot generator build + render:

```bash
./scripts/run_screenshot_generator.sh
# or with explicit sources dir
./scripts/run_screenshot_generator.sh /path/to/sources
```

Logs are written under `logs/` with timestamp-first naming.

### TODO (upstream: seedsigner-c-modules)

- Fix `tests/screenshot_generator/CMakeLists.txt` in `kdmukai-bot/seedsigner-c-modules` to avoid hardcoded `references/micropython/.../lvgl__lvgl` path assumptions.
- Preferred approach: allow `LVGL_ROOT` override (or auto-detect from current workspace) and only use the legacy path as optional fallback.


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

- `builder_ref` — branch/tag/SHA of this repo (`seedsigner-micropython-builder`) to run.
- `c_modules_ref` — branch/tag/SHA of `kdmukAI-bot/seedsigner-c-modules` to use.

Defaults:

- `builder_ref = main`
- `c_modules_ref = master`

Use this to test feature branches of either repo without changing default CI behavior.

## Prebuilt base image (GHCR)

To reduce repeated setup work, CI uses a prebaked base image with:

- pinned MicroPython baseline (including submodules + prebuilt mpy-cross)
- pinned ESP-IDF baseline (tools installed)
- common host/build dependencies
- fixed `IDF_TOOLS_PATH=/opt/espressif` to avoid runtime `HOME` differences
- default ccache env (`CCACHE_DIR=/workspace/.ccache`, `CCACHE_BASEDIR=/workspace`, `CCACHE_COMPILERCHECK=content`)
- build-time validation that `idf.py --version` works

Build/publish this image via workflow:

- `.github/workflows/build-base-image.yml`

Firmware workflow then seeds `sources/` from that image and applies project mods before build.
