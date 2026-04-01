# SeedSigner MicroPython Builder — Project Instructions

## What This Project Is
Build orchestration for compiling MicroPython v1.27.0 firmware targeting ESP32-S3 and ESP32-P4 boards. This repo does **not** fork MicroPython or ESP-IDF — it applies patches and overlays on top of clean upstream baselines, maintaining reproducibility without long-lived forks.

### Key Architecture
- **MicroPython**: Git submodule at `deps/micropython/upstream/` pinned to `v1.27.0`. Patched at build time, restored to clean state after.
- **ESP-IDF v5.5.1**: Prebaked in Docker image at `/opt/toolchains/esp-idf`
- **seedsigner-c-modules**: Git submodule at `deps/seedsigner-c-modules/` — version-pinned
- **ESP32 components**: `ports/esp32/` — hardware drivers (display, camera, power, BSP)
- **MicroPython bindings**: `bindings/` + `usercmodule.cmake` — C module integration

### Build Flow
```
prepare_sources_from_image.sh  →  Verifies submodules + prebaked ESP-IDF
apply_micropython_mods.sh      →  Applies patches + overlays new board files
apply_idf_mods.sh              →  (placeholder, no IDF patches yet)
build_firmware.sh              →  Builds mpy-cross, then ESP32 firmware
run_screenshot_generator.sh    →  Builds LVGL screenshot gallery from c-modules
restore_micropython_clean.sh   →  Restores submodule to clean pinned state
```

### Patch System (`deps/micropython/mods/`)
- `patches/0001-esp32-integration-mods.patch` — Modifies 5 MicroPython files: adds external component dirs, display_manager integration, LVGL dependencies, network stripping option
- `new_files/` — Board definitions (mpconfigboard.h, sdkconfig, partition table) for supported boards
- Patches are generated against the clean pinned commit and apply with plain `git apply` (no `--3way`)

### MicroPython Patch Workflow

**Scripts:**
| Script | Purpose |
|---|---|
| `scripts/apply_micropython_mods.sh` | Apply patches + new_files overlay, commit result |
| `scripts/restore_micropython_clean.sh` | Reset submodule to clean pinned commit |
| `scripts/generate_micropython_patch.sh` | Regenerate patch from current tree, verify round-trip |

**Iterating on MicroPython changes:**
1. Apply the current patches: `scripts/apply_micropython_mods.sh`
2. Edit files under `deps/micropython/upstream/ports/`
3. Build with dirty tree: `MP_ALLOW_DIRTY=1 make docker-build-all`
4. Repeat steps 2-3 until satisfied
5. Regenerate the patch: `scripts/generate_micropython_patch.sh`
6. Restore clean state: `scripts/restore_micropython_clean.sh`

**Important:** A clean `make docker-build-all` applies patches automatically and restores the submodule on exit. If the submodule is already dirty (patched or edited), the build fails unless `MP_ALLOW_DIRTY=1` is set — this prevents accidental builds on a stale or unknown tree.

### Build Targets
```bash
make docker-shell                     # Interactive shell in build container
make docker-build-all                 # Full build: setup + firmware + screenshots
BOARD=WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43 make docker-build-all  # Build for a specific board
MP_ALLOW_DIRTY=1 make docker-build-all  # Build with local MicroPython edits
make clean                            # Remove build artifacts
make full-reset CONFIRM=YES           # Remove all ephemeral deps + artifacts
```

### Output
- `build/<BOARD>/` — `micropython.bin`, `.elf`, bootloader, partition table, `flash_args`
- Flash via: `python -m esptool --chip <chip> write_flash @flash_args`

### Supported Boards
| Board | Chip | Display | Flash |
|---|---|---|---|
| `WAVESHARE_ESP32_S3_TOUCH_LCD_35B` (default) | ESP32-S3 | AXS15231B QSPI 320x480 | 16MB |
| `WAVESHARE_ESP32_S3_TOUCH_LCD_35` | ESP32-S3 | ST7796 SPI 320x480 | 16MB |
| `WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_35` | ESP32-P4 | ST7796 SPI 320x480 | 16MB |
| `WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43` | ESP32-P4 | ST7701 MIPI-DSI 480x800 | 32MB |

All boards: Network/Bluetooth disabled at both MicroPython and IDF level.

## Broader Ecosystem Context
This repo builds ESP32-S3 and ESP32-P4 firmware variants of SeedSigner. It takes the shared LVGL screens from `seedsigner-c-modules`, combines them with ESP32-specific hardware drivers from `ports/esp32/`, and bakes them into MicroPython firmware images that will eventually run the SeedSigner Python business logic on microcontrollers.

### The Four Repos (all under `/home/kdmukai/dev/`)

1. **`seedsigner/`** — Production Python application (business logic, views, models). The goal is to make this codebase MicroPython 1.27.0-compatible so it runs on the firmware this repo produces.

2. **`seedsigner-c-modules/`** — Shared LVGL screens (C/C++). This repo consumes it as a git submodule for the platform-agnostic screen code (`components/seedsigner/`, `components/nlohmann_json/`).

3. **`seedsigner-micropython-builder/`** (this repo) — Compiles MicroPython + c-modules + ESP32 hardware drivers into flashable ESP32-S3 firmware. Owns the MicroPython bindings (`bindings/`, `usercmodule.cmake`) and ESP32-specific components (`ports/esp32/`). Docker-only builds for reproducibility.

4. **`seedsigner-raspi-lvgl/`** — The Pi Zero counterpart: compiles the same LVGL screens from `seedsigner-c-modules` into a CPython extension. Not used by this repo, but produces the equivalent rendering layer for the other platform.

### How They Connect
```
seedsigner-c-modules (git submodule — shared LVGL screens)
       │
       ▼
seedsigner-micropython-builder (this repo)
       │  patches MicroPython, adds board def,
       │  compiles firmware with LVGL C modules
       │  + ESP32 hardware drivers (ports/esp32/)
       │  + MicroPython bindings (bindings/)
       ▼
ESP32-S3 firmware (micropython.bin)
       │
       ▼
Runs seedsigner Python business logic on MicroPython 1.27.0
```

## CI/CD
- **GitHub** (`.github/workflows/`) — Full featured: firmware build, artifacts, Pages deployment with PR previews. Base image from GHCR.
- **GitLab** (`.gitlab-ci.yml`) — Firmware build + GitLab Pages. Base image from GitLab Container Registry.
- **Forgejo/Codeberg** (`.forgejo/workflows/`) — Firmware build + branch-based Pages. Base image from Codeberg registry.
- **Shared CI logic** (`scripts/ci/ci.sh`) — Common build steps called by all platform configs.
- **`build-base-image.yml`** — Builds/publishes Docker image with prebaked MicroPython + ESP-IDF (GitHub-only, images mirrored to other registries manually).

<!-- Git, Builds, and General rules are in the parent /home/kdmukai/dev/CLAUDE.md -->
