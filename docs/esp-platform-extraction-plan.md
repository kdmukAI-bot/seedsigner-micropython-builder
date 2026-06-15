# ESP Platform Extraction Plan

## Context

`seedsigner-lvgl-screens` currently contains both **shared** LVGL screen code and **ESP32-specific** hardware drivers + MicroPython bindings. The raspi-lvgl repo only uses the shared code (`components/seedsigner/`, `components/nlohmann_json/`, `third_party/lvgl`, `tools/`). The ESP-specific code is only consumed by this builder repo. Moving it here makes the dependency boundaries match the actual platform split.

This refactor also improves the repo's directory naming:
- `sources/` → `deps/` (external dependencies, not "source code")
- `platform_mods/` → nested under `deps/` alongside the things they modify
- ESP components organized under `ports/esp32/` (MicroPython convention)

### Sequencing Decision: v8 First, v9 Later

An LVGL v8 → v9 migration is in progress in lvgl-screens (PR #12, branch `feature/lvgl-v9-migration`). We restructure against v8 first because:

1. **Separation of concerns** — the restructure is mechanical (move files, update paths); the v9 rewrite is functional (new APIs, hardware-specific flush callback). Mixing them makes debugging harder.
2. **PR #12 is still open** — building on an unmerged branch is fragile.
3. **Hardware testing** — the v9 display rewrite (AXS15231B QSPI direct_mode flush) can't be validated without flashing real hardware.
4. **No conflicts** — our `remove-esp-platform` branch and PR #12 don't touch the same files in lvgl-screens.

After this restructure lands:
1. Merge PR #12 into lvgl-screens `main`
2. Update micropython-builder submodule to new lvgl-screens `main`
3. Do v9 display rewrite in micropython-builder (see `seedsigner-lvgl-screens/docs/lvgl-v9-migration-plan.md` Parts 4a–4e for detailed instructions)

## What Moves

**ESP platform components** → `ports/esp32/`:

| From lvgl-screens | To micropython-builder |
|---|---|
| `components/esp_bsp/` | `ports/esp32/esp_bsp/` |
| `components/esp_lv_port/` | `ports/esp32/esp_lv_port/` |
| `components/display_manager/` | `ports/esp32/display_manager/` |
| `components/esp32-camera/` | `ports/esp32/esp32-camera/` |
| `components/XPowersLib/` | `ports/esp32/XPowersLib/` |

**MicroPython bindings** → repo root:

| From lvgl-screens | To micropython-builder |
|---|---|
| `bindings/*` | `bindings/*` |
| `usercmodule.cmake` | `usercmodule.cmake` |

**Stays in lvgl-screens**: `components/seedsigner/`, `components/nlohmann_json/`, `third_party/lvgl`, `tools/`, `scripts/`, `tests/`, `docs/`

## Final Directory Layout

```
seedsigner-micropython-builder/
  usercmodule.cmake                      # MicroPython entry point
  bindings/                              # MicroPython integration layer
    micropython.cmake
    modseedsigner_bindings.c
    moddisplay_manager_bindings.c
  ports/
    esp32/                               # ESP32 hardware components (flat)
      display_manager/
      esp_bsp/
      esp_lv_port/
      esp32-camera/
      XPowersLib/
  deps/                                  # external dependencies (was sources/)
    micropython/
      upstream/                          # ephemeral MicroPython checkout
      mods/                              # was platform_mods/micropython_mods/
        patches/
        new_files/
        BASELINE
        README.md
    esp-idf/
      mods/                              # was platform_mods/idf_mods/
        BASELINE
    seedsigner-lvgl-screens/                # git submodule
  scripts/
  build/
  Makefile
```

## Implementation Steps

### Step 1: Create lvgl-screens feature branch

Branch: `remove-esp-platform` off lvgl-screens `main`

- Delete: `components/esp_bsp/`, `components/esp_lv_port/`, `components/display_manager/`, `components/esp32-camera/`, `components/XPowersLib/`
- Delete: `bindings/` directory
- Delete: `usercmodule.cmake`
- Commit

### Step 2: Add new files to micropython-builder

**`ports/esp32/`** — copy all 5 ESP component directories verbatim from lvgl-screens main.

**`bindings/`** — copy .c files verbatim; modify `micropython.cmake`:
```cmake
target_include_directories(usermod_dm INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/display_manager
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/esp_bsp
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/esp_lv_port/include
    ${SEEDSIGNER_LVGL_SCREENS_DIR}/components/seedsigner
)
```

**`usercmodule.cmake`** — at repo root:
```cmake
include(${CMAKE_CURRENT_LIST_DIR}/bindings/micropython.cmake)
```

### Step 3: Restructure `sources/` → `deps/` with nested mods

Move and reorganize:
```
sources/micropython/         →  deps/micropython/upstream/
sources/seedsigner-lvgl-screens →  deps/seedsigner-lvgl-screens/
platform_mods/micropython_mods/ → deps/micropython/mods/
platform_mods/idf_mods/      →  deps/esp-idf/mods/
```

Update `.gitmodules`:
```
[submodule "deps/seedsigner-lvgl-screens"]
    path = deps/seedsigner-lvgl-screens
    url = https://github.com/kdmukAI-bot/seedsigner-lvgl-screens.git
```

Note: Changing a submodule path requires `git rm sources/seedsigner-lvgl-screens` then re-adding at the new path.

### Step 4: Update all scripts for new paths

**Common pattern change**: Scripts use `WORKDIR="${1:-$ROOT_DIR/sources}"`. Change default to `$ROOT_DIR/deps` and update derived paths.

**`scripts/build_firmware.sh`** (core changes):
```bash
WORKDIR="${1:-$ROOT_DIR/deps}"
MP_DIR="$WORKDIR/micropython/upstream"      # was $WORKDIR/micropython
SCREENS_DIR="$WORKDIR/seedsigner-lvgl-screens"

PORTS_ESP32_DIR="$ROOT_DIR/ports/esp32"
USER_C_MODULES_FILE="$ROOT_DIR/usercmodule.cmake"
MICROPY_CMAKE_ARGS="${CMAKE_ARGS:-} -DUSER_C_MODULES=$USER_C_MODULES_FILE"
MICROPY_CMAKE_ARGS="$MICROPY_CMAKE_ARGS -DMICROPY_EXTRA_COMPONENT_DIRS=${PORTS_ESP32_DIR}\;${SCREENS_DIR}/components"
MICROPY_CMAKE_ARGS="$MICROPY_CMAKE_ARGS -DSEEDSIGNER_LVGL_SCREENS_DIR=$SCREENS_DIR"
```

**`scripts/apply_micropython_mods.sh`**:
- `MODS_DIR="$ROOT_DIR/deps/micropython/mods"` (was `$ROOT_DIR/platform_mods/micropython_mods`)
- `MP_DIR="$WORKDIR/micropython/upstream"` (was `$WORKDIR/micropython`)

**`scripts/prepare_sources_from_image.sh`**:
- Seed to `$WORKDIR/micropython/upstream` (was `$WORKDIR/micropython`)
- BASELINE at `$ROOT_DIR/deps/micropython/mods/BASELINE`

**`scripts/verify_micropython_base.sh`**:
- `MP_DIR="$WORKDIR/micropython/upstream"`
- `BASELINE_FILE="$ROOT_DIR/deps/micropython/mods/BASELINE"`

**`scripts/verify_idf_base.sh`** (placeholder):
- `BASELINE_FILE="$ROOT_DIR/deps/esp-idf/mods/BASELINE"`

**`scripts/run_screenshot_generator.sh`**:
- `SCREENS_DIR="$WORKDIR/seedsigner-lvgl-screens"` (same relative path, just under deps/)
- LVGL path: `$WORKDIR/micropython/upstream/ports/esp32/managed_components/lvgl__lvgl`

**`scripts/docker_build_all.sh`**:
- `mkdir -p deps` (was `sources`)
- All `sources/` refs → `deps/`
- lvgl-screens clone to `$ROOT_DIR/deps/seedsigner-lvgl-screens`

**`scripts/ci_build.sh`**:
- WORKDIR default to `$ROOT_DIR/deps`

**`scripts/setup_env.sh`**:
- WORKDIR default to `$ROOT_DIR/deps`

**`Makefile`**:
- `clean` target: `deps/micropython/upstream/ports/esp32/build*` and `deps/seedsigner-lvgl-screens/tools/...`
- `full-reset`: `rm -rf deps build logs .ccache`
- Comment updates

**`.github/workflows/build-firmware.yml`**:
- `mkdir -p deps` and `prepare_sources_from_image.sh "$PWD/deps"`
- `git -C deps/seedsigner-lvgl-screens ...`
- `git -C deps/micropython/upstream ...`
- Screenshot copy from `deps/seedsigner-lvgl-screens/tools/...`

### Step 5: Update MicroPython patch for multi-directory support

In `deps/micropython/mods/patches/0001-esp32-integration-mods.patch`, change the `MICROPY_EXTRA_COMPONENT_DIRS` block from single-path to foreach:

```cmake
if(DEFINED MICROPY_EXTRA_COMPONENT_DIRS)
    foreach(_ext_comp_dir ${MICROPY_EXTRA_COMPONENT_DIRS})
        if(EXISTS "${_ext_comp_dir}")
            list(APPEND EXTRA_COMPONENT_DIRS "${_ext_comp_dir}")
        else()
            message(WARNING "MICROPY_EXTRA_COMPONENT_DIRS entry not found: ${_ext_comp_dir}")
        endif()
    endforeach()
endif()
```

**Approach**: Apply old patch to clean MicroPython, make the edit, regenerate with `git diff`.

### Step 6: Point submodule to lvgl-screens feature branch

```bash
git rm sources/seedsigner-lvgl-screens
git submodule add https://github.com/kdmukAI-bot/seedsigner-lvgl-screens.git deps/seedsigner-lvgl-screens
cd deps/seedsigner-lvgl-screens
git checkout remove-esp-platform
cd ../..
git add deps/seedsigner-lvgl-screens .gitmodules
```

### Step 7: Test build

Run `make docker-build-all` to verify:
1. Both `EXTRA_COMPONENT_DIRS` entries register correctly
2. Cross-directory component resolution works (`display_manager` REQUIRES `seedsigner` across dirs)
3. MicroPython bindings compile with `SEEDSIGNER_LVGL_SCREENS_DIR`
4. Screenshot generator still works

### Step 8: Land PRs

1. PR `remove-esp-platform` branch into lvgl-screens `main`
2. After merge, update submodule pin to lvgl-screens `main`
3. PR the micropython-builder changes

## Follow-up: LVGL v9 Migration — COMPLETED

Branch: `feature/lvgl-v9-migration` (PR pending — GitHub account suspended as of 2026-03-20)

### What was done

1. Updated lvgl-screens submodule to v9 (`2256fbd`)
2. **Deleted `ports/esp32/esp_lv_port/`** — replaced by `espressif/esp_lvgl_port` v2.7.2
3. **Rewrote `ports/esp32/display_manager/display_manager.cpp`** for v9
4. **Deleted custom touch** (`bsp_touch.c/h`) — replaced by `esp_lcd_touch_new_i2c_axs15231b()`
5. Updated `bindings/micropython.cmake`, `sdkconfig.board`, MicroPython patch
6. All ESP-IDF component dependencies pinned to exact versions

### Additional changes bundled in the same branch

- **MicroPython as git submodule** — replaced ephemeral Docker copy with submodule at `deps/micropython/upstream` pinned to v1.27.0. Patch script commits after applying for clean dev workflow. Only 3 nested submodules needed: `lib/berkeley-db-1.xx`, `lib/micropython-lib`, `lib/tinyusb`.
- **Docker cleanup** — `--user` + `--tmpfs /tmp/home` + `~/.cache` bind mount. No root-owned artifacts. Flash package at `build/<board>/flash/`.

### AXS15231B display — critical discoveries

These findings are specific to the Waveshare ESP32-S3 Touch LCD 3.5B (AXS15231B QSPI):

1. **Race condition with `esp_lvgl_port`**: `lvgl_port_add_disp()` registers a default flush callback that sends partial `draw_bitmap` calls. If the LVGL task runs a frame before the custom callback override takes effect, the partial draws corrupt the panel's write pointer (RASET bug). **Fix**: hold `lvgl_port_lock()` across `lvgl_port_add_disp()` and both callback overrides.

2. **`lv_display_set_rotation()` is broken in LVGL v9**: Does not transform rendering coordinates in either `direct_mode` or `full_refresh` mode. Produces a blank screen with a column of random pixels. Tested both modes — neither works.

3. **Rotation must be done in the flush callback**: LVGL renders landscape (480×320) into a SPIRAM framebuffer via `direct_mode`. The flush callback rotates 90° CW to portrait (320×480) while copying into DMA bounce buffers. Byte swap (RGB565 endianness) is done in the same per-pixel loop. Cache-optimized loop order: panel column (px) outer, band row (by) inner — makes SPIRAM reads sequential along framebuffer rows.

4. **Touch mapping**: `swap_xy=1, mirror_x=1, mirror_y=0` with physical portrait `x_max=320, y_max=480`.

5. **LVGL task stack**: 10KB required (was 5KB). Scroll animations + rotation flush callback exceed 5KB stack depth.

6. **Screensaver animation is choppy**: Inherent to the per-pixel rotation overhead (~6ms CPU per frame). Not an issue for SeedSigner's mostly-static UI. Other displays without the RASET bug would not have this limitation.

### Known limitation: rendering performance

The RASET bug forces full-frame DMA on every flush. Combined with software rotation in the flush callback, this adds ~6ms CPU overhead per frame. The `full_refresh` + `lv_display_set_rotation()` approach (which would eliminate the rotation overhead) was tested and does not work due to the v9 rotation bug.

Future optimization options:
- Cached portrait buffer (only re-rotate dirty bands)
- Background rotation on second core
- Custom LVGL draw backend with built-in rotation
- Accept the tradeoff (SeedSigner UI is mostly static)

## Risks & Mitigations

1. **Shell quoting of semicolons**: `MICROPY_EXTRA_COMPONENT_DIRS` passes through Make → CMake. The `\;` escape should work; fallback is two separate `-D` flags.
2. **Cross-directory REQUIRES**: `display_manager` depends on `seedsigner` in a different component dir. ESP-IDF collects all dirs before resolving — should work. Verify in Step 7.
3. **Lock file staleness**: The `dependencies.lock.esp32s3` may need regenerating. If build fails, delete lock file, let it regenerate, update patch.
4. **Submodule path change**: Changing from `sources/seedsigner-lvgl-screens` to `deps/seedsigner-lvgl-screens` requires `git rm` + `git submodule add` (not just a rename).
