# seedsigner-micropython-builder

Build orchestration repo for SeedSigner MicroPython firmware without maintaining long-lived forks.

## Supported hardware

| Board | Manufacturer | Screen | Price | Status |
|---|---|---|---|---|
| [ESP32-P4-WIFI6-Touch-LCD-4.3](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4.3.htm) | Waveshare | 4.3″ | [$43](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4.3.htm?sku=33875) | ✅ Supported · ⭐ **Recommended** |
| [JC4880P443](https://www.aliexpress.us/item/3256809431944589.html) | Guition | 4.3″ | $35 | ✅ Supported |
| [ESP32-P4-WIFI6-Touch-LCD-3.5](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-3.5.htm) | Waveshare | 3.5″ | [$38](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-3.5.htm?sku=33511) | ✅ Supported |
| [CrowPanel Advance 5″ ESP32-P4](https://www.elecrow.com/crowpanel-advanced-5inch-esp32-p4-hmi-ai-display-800x480-ips-touch-screen-with-wifi-6.html) | Elecrow | 5″ | $32 | 🛠️ **Bring-up in progress** |
| [ESP32-P4-WIFI6-Touch-LCD-5](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-5.htm) | Waveshare | 5″ | [$53](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-5.htm?sku=33763) | ⏸️ Paused |
| [ESP32-S3-Touch-LCD-3.5B](https://www.waveshare.com/esp32-s3-touch-lcd-3.5b.htm) | Waveshare | 3.5″ | — | 🔬 Under evaluation |

The Elecrow CrowPanel is the first board here with a **landscape-native panel** (800×480 parallel
RGB), so it needs none of the per-frame rotation the portrait boards pay for. Bring-up plan and
status: [`docs/elecrow-crowpanel-p4-5in-bringup-plan.md`](docs/elecrow-crowpanel-p4-5in-bringup-plan.md).
The Waveshare 5″ is paused behind two unresolved defects — see
[`docs/knowledge/esp32-p4-dsi-rotation-cost-scaling.md`](docs/knowledge/esp32-p4-dsi-rotation-cost-scaling.md).

Prices are list prices as of July 2026. See **[Board hardware details](docs/supported-hardware.md)**
for per-board specs, enclosure options, and the display / performance differences.

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

# Build-critical submodules, recursively (seedsigner-lvgl-screens pulls LVGL,
# micropython pulls micropython-lib):
git submodule update --init --recursive \
    deps/micropython/upstream deps/seedsigner-lvgl-screens \
    deps/cUR deps/esp-secp256k1 ports/esp32/board_common

# Frozen-app sources, NON-recursively — the app + embit are frozen into the image.
# Their own nested submodules (translations, screenshots) aren't needed: the freeze
# excludes resources/, so skip the recursion (and the large translations clone).
git submodule update --init deps/seedsigner deps/embit
```

`--recursive` is required for `seedsigner-lvgl-screens` (its own LVGL submodule) and
`micropython` (micropython-lib). The app (`deps/seedsigner`) and `embit` (`deps/embit`) are
the sources frozen into the firmware — init them non-recursively.

> Quick-and-dirty: a plain `git submodule update --init --recursive` from the repo root also
> works, but additionally clones the app's `seedsigner-translations` submodule, which the
> frozen build never uses.

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

### `deps/seedsigner` + `deps/embit` — frozen-app sources (branch-tracked submodules)

The SeedSigner app and `embit` are frozen into the firmware, so they are tracked as submodules
to give the build a deterministic, CI-clonable source:

- `deps/seedsigner` → `kdmukAI-bot/seedsigner`, tracking `integration/lvgl-mpy`.
- `deps/embit` → `kdmukAI-bot/embit`, tracking `perf/psbt-parse-speedups-v0.8.0` (embit v0.8.0 —
  the version the app's `requirements.txt` pins — plus a BIP32 derivation-hot-path perf commit).

Both carry a `branch =` in `.gitmodules`, so the pin follows a branch tip rather than a frozen
commit. Advance a pin to its current tip with:

```bash
git submodule update --remote deps/seedsigner    # bump to the branch tip
git add deps/seedsigner && git commit -m "bump app pin"
```

**Local dev builds off your sibling working trees, not these submodules.** The stager
(`tools/stage_frozen_app.py`) resolves the app + embit source via `tools/_devenv` (`SS_APP_DIR` /
`SS_EMBIT_DIR`, `.env`-overridable), defaulting to the sibling checkouts `../seedsigner` and
`../embit` — so you iterate on your live tree. CI points those env vars at these submodules for a
reproducible build. See [README-dev.md](README-dev.md#frozen-app-build--versioning).

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
- **Auto-stages the frozen app.** `docker-build-all` depends on `make stage-app`, which mirrors
  the app + embit into `frozen_app/` and bakes the version before the container build freezes it
  (see [README-dev.md](README-dev.md#frozen-app-build--versioning)). Staging fails clearly if the
  app/embit sources aren't found — set `SS_APP_DIR` / `SS_EMBIT_DIR` (or `.env`), or init the
  `deps/seedsigner` + `deps/embit` submodules.
- First run: initialize submodules (see [Cloning and submodule setup](#cloning-and-submodule-setup)) if `deps/micropython/upstream` is empty.
- Subsequent runs: if `deps/micropython/upstream` is dirty, the build uses your current working tree and skips repatching.

## CI

GitHub Actions workflow: `.github/workflows/build-firmware.yml`

CI checks out this repo with `submodules: true` (populating `seedsigner-lvgl-screens` and
`deps/micropython/upstream` at their pinned commits), applies mods, builds firmware, and
uploads artifacts.

**Pull requests** run a fast compile-only firmware check for all three supported ESP32-P4 boards —
the Waveshare 4.3″, the Guition JC4880P443, and the Waveshare 3.5″ (the app is not frozen in). **On
merge to `main`**, GitHub CI additionally stages the frozen app from the pinned `deps/seedsigner` /
`deps/embit` submodules, builds it in, and publishes a downloadable **`flashable-dist-*`** artifact
for each — self-booting images anyone can flash (see below). GitLab / Codeberg build the ESP32-S3 dev
boards and skip the frozen-app bake for now.


## Build outputs

- Firmware build directory (default): `build/<BOARD>/`
- Flash-ready package (from `make dist`): `dist/<BOARD>/`
- Staged frozen app (gitignored, regenerated by `make stage-app`): `frozen_app/`
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


## Build & flash your own firmware

There are two ways to get a flashable image.

**1. Download a prebuilt image (easiest).** On merge to `main`, CI publishes a **`flashable-dist-*`**
artifact for each of the three supported ESP32-P4 boards — the Waveshare 4.3″ (primary), the Guition
JC4880P443, and the Waveshare 3.5″ (see [CI](#ci)). Download and unzip the one for your board — it
contains the same `dist/<BOARD>/` layout as below and self-boots — then skip to [flashing](#flashing).

**2. Build it yourself.** Once the submodules are set up (see
[Cloning and submodule setup](#cloning-and-submodule-setup)), building a flashable image is two
commands — `docker-build-all` auto-stages the frozen app, and `make dist` packages the flash-ready
artifacts into `dist/<BOARD>/`:

```bash
make docker-build-all BOARD=WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43
make dist             BOARD=WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43
```

`dist/<BOARD>/` then holds `micropython.bin`, `bootloader/`, `partition_table/`, `vfs.bin` (the baked
`/main.py` launcher — see below), and the generated `flash_args` manifest that flashes all of them.
(The raw build tree is also left under `build/<BOARD>/`.)

| Board | Chip |
|---|---|
| `WAVESHARE_ESP32_S3_TOUCH_LCD_35B`, `WAVESHARE_ESP32_S3_TOUCH_LCD_35` | `esp32s3` |
| `WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_35`, `WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43` | `esp32p4` |
| `GUITION_JC4880P443`, `ELECROW_CROWPANEL_ADV_P4_50` | `esp32p4` |

### Flashing

Flashing runs on your **host**, not in the build container — it needs USB access to the board — so
install the host-side Python tools first: `esptool` (the flasher) and `pyserial` (used by the
`tools/` provisioning scripts). With Python 3:

```bash
pip install -r tools/requirements.txt   # or: pip install esptool pyserial
```

Flash from `dist/<BOARD>/`, using the generated offsets:

```bash
cd dist/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43
python -m esptool --chip esp32p4 --port /dev/ttyACM0 write_flash @flash_args
```

**Safety:**
- **Identify the board first.** Serial ports re-enumerate between sessions (especially on a
  multi-board bench) — run `python -m esptool --port <port> flash_id` and match the chip + flash
  size before writing, so you don't flash the wrong board.
- **Never pipe `write_flash`** (e.g. `| head`) — a SIGPIPE mid-write can leave a partially flashed
  image.

**Launching the app.** A `dist/` flash **self-boots** — `make dist` bakes a `/main.py` launcher into
`vfs.bin`, which `flash_args` writes into the board's internal filesystem, so a freshly flashed board
boots straight into the app on first power-up. No extra step is needed.

The exception is a **firmware-only** package (`BAKE_LAUNCHER=0 make dist`, or flashing just
`micropython.bin`): with no `/main.py`, the board boots to the REPL. Provision the launcher over
serial with:

```bash
python3 tools/set_p4_boot_app.py --port /dev/ttyACM0
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
