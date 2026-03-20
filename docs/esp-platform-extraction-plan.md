# ESP Platform Extraction Plan

## Context

`seedsigner-c-modules` currently contains both **shared** LVGL screen code and **ESP32-specific** hardware drivers + MicroPython bindings. The raspi-lvgl repo only uses the shared code (`components/seedsigner/`, `components/nlohmann_json/`, `third_party/lvgl`, `tools/`). The ESP-specific code is only consumed by this builder repo. Moving it here makes the dependency boundaries match the actual platform split.

This refactor also improves the repo's directory naming:
- `sources/` → `deps/` (external dependencies, not "source code")
- `platform_mods/` → nested under `deps/` alongside the things they modify
- ESP components organized under `ports/esp32/` (MicroPython convention)

### Sequencing Decision: v8 First, v9 Later

An LVGL v8 → v9 migration is in progress in c-modules (PR #12, branch `feature/lvgl-v9-migration`). We restructure against v8 first because:

1. **Separation of concerns** — the restructure is mechanical (move files, update paths); the v9 rewrite is functional (new APIs, hardware-specific flush callback). Mixing them makes debugging harder.
2. **PR #12 is still open** — building on an unmerged branch is fragile.
3. **Hardware testing** — the v9 display rewrite (AXS15231B QSPI direct_mode flush) can't be validated without flashing real hardware.
4. **No conflicts** — our `remove-esp-platform` branch and PR #12 don't touch the same files in c-modules.

After this restructure lands:
1. Merge PR #12 into c-modules `main`
2. Update micropython-builder submodule to new c-modules `main`
3. Do v9 display rewrite in micropython-builder (see `seedsigner-c-modules/docs/lvgl-v9-migration-plan.md` Parts 4a–4e for detailed instructions)

## What Moves

**ESP platform components** → `ports/esp32/`:

| From c-modules | To micropython-builder |
|---|---|
| `components/esp_bsp/` | `ports/esp32/esp_bsp/` |
| `components/esp_lv_port/` | `ports/esp32/esp_lv_port/` |
| `components/display_manager/` | `ports/esp32/display_manager/` |
| `components/esp32-camera/` | `ports/esp32/esp32-camera/` |
| `components/XPowersLib/` | `ports/esp32/XPowersLib/` |

**MicroPython bindings** → repo root:

| From c-modules | To micropython-builder |
|---|---|
| `bindings/*` | `bindings/*` |
| `usercmodule.cmake` | `usercmodule.cmake` |

**Stays in c-modules**: `components/seedsigner/`, `components/nlohmann_json/`, `third_party/lvgl`, `tools/`, `scripts/`, `tests/`, `docs/`

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
    seedsigner-c-modules/                # git submodule
  scripts/
  build/
  Makefile
```

## Implementation Steps

### Step 1: Create c-modules feature branch

Branch: `remove-esp-platform` off c-modules `main`

- Delete: `components/esp_bsp/`, `components/esp_lv_port/`, `components/display_manager/`, `components/esp32-camera/`, `components/XPowersLib/`
- Delete: `bindings/` directory
- Delete: `usercmodule.cmake`
- Commit

### Step 2: Add new files to micropython-builder

**`ports/esp32/`** — copy all 5 ESP component directories verbatim from c-modules main.

**`bindings/`** — copy .c files verbatim; modify `micropython.cmake`:
```cmake
target_include_directories(usermod_dm INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/display_manager
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/esp_bsp
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/esp_lv_port/include
    ${SEEDSIGNER_C_MODULES_DIR}/components/seedsigner
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
sources/seedsigner-c-modules →  deps/seedsigner-c-modules/
platform_mods/micropython_mods/ → deps/micropython/mods/
platform_mods/idf_mods/      →  deps/esp-idf/mods/
```

Update `.gitmodules`:
```
[submodule "deps/seedsigner-c-modules"]
    path = deps/seedsigner-c-modules
    url = https://github.com/kdmukAI-bot/seedsigner-c-modules.git
```

Note: Changing a submodule path requires `git rm sources/seedsigner-c-modules` then re-adding at the new path.

### Step 4: Update all scripts for new paths

**Common pattern change**: Scripts use `WORKDIR="${1:-$ROOT_DIR/sources}"`. Change default to `$ROOT_DIR/deps` and update derived paths.

**`scripts/build_firmware.sh`** (core changes):
```bash
WORKDIR="${1:-$ROOT_DIR/deps}"
MP_DIR="$WORKDIR/micropython/upstream"      # was $WORKDIR/micropython
CMODS_DIR="$WORKDIR/seedsigner-c-modules"

PORTS_ESP32_DIR="$ROOT_DIR/ports/esp32"
USER_C_MODULES_FILE="$ROOT_DIR/usercmodule.cmake"
MICROPY_CMAKE_ARGS="${CMAKE_ARGS:-} -DUSER_C_MODULES=$USER_C_MODULES_FILE"
MICROPY_CMAKE_ARGS="$MICROPY_CMAKE_ARGS -DMICROPY_EXTRA_COMPONENT_DIRS=${PORTS_ESP32_DIR}\;${CMODS_DIR}/components"
MICROPY_CMAKE_ARGS="$MICROPY_CMAKE_ARGS -DSEEDSIGNER_C_MODULES_DIR=$CMODS_DIR"
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
- `CMODS_DIR="$WORKDIR/seedsigner-c-modules"` (same relative path, just under deps/)
- LVGL path: `$WORKDIR/micropython/upstream/ports/esp32/managed_components/lvgl__lvgl`

**`scripts/docker_build_all.sh`**:
- `mkdir -p deps` (was `sources`)
- All `sources/` refs → `deps/`
- c-modules clone to `$ROOT_DIR/deps/seedsigner-c-modules`

**`scripts/ci_build.sh`**:
- WORKDIR default to `$ROOT_DIR/deps`

**`scripts/setup_env.sh`**:
- WORKDIR default to `$ROOT_DIR/deps`

**`Makefile`**:
- `clean` target: `deps/micropython/upstream/ports/esp32/build*` and `deps/seedsigner-c-modules/tools/...`
- `full-reset`: `rm -rf deps build logs .ccache`
- Comment updates

**`.github/workflows/build-firmware.yml`**:
- `mkdir -p deps` and `prepare_sources_from_image.sh "$PWD/deps"`
- `git -C deps/seedsigner-c-modules ...`
- `git -C deps/micropython/upstream ...`
- Screenshot copy from `deps/seedsigner-c-modules/tools/...`

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

### Step 6: Point submodule to c-modules feature branch

```bash
git rm sources/seedsigner-c-modules
git submodule add https://github.com/kdmukAI-bot/seedsigner-c-modules.git deps/seedsigner-c-modules
cd deps/seedsigner-c-modules
git checkout remove-esp-platform
cd ../..
git add deps/seedsigner-c-modules .gitmodules
```

### Step 7: Test build

Run `make docker-build-all` to verify:
1. Both `EXTRA_COMPONENT_DIRS` entries register correctly
2. Cross-directory component resolution works (`display_manager` REQUIRES `seedsigner` across dirs)
3. MicroPython bindings compile with `SEEDSIGNER_C_MODULES_DIR`
4. Screenshot generator still works

### Step 8: Land PRs

1. PR `remove-esp-platform` branch into c-modules `main`
2. After merge, update submodule pin to c-modules `main`
3. PR the micropython-builder changes

## Follow-up: LVGL v9 Migration (separate PR)

After this restructure lands and c-modules PR #12 merges:

1. Update submodule to c-modules `main` (now with v9 screens)
2. **Delete `ports/esp32/esp_lv_port/`** — replaced by `espressif/esp_lvgl_port` v2.7.2 registry component
3. **Rewrite `ports/esp32/display_manager/`** for v9:
   - Use `esp_lvgl_port` for display init (`lvgl_port_add_disp()`)
   - Touch via `esp_lcd_touch_new_i2c_axs15231b()` + `lvgl_port_add_touch()`
   - AXS15231B QSPI `direct_mode` flush callback (see migration plan Part 4b)
   - Public API (`init()`, `run_screen()`) unchanged — bindings need no changes
4. **Remove custom touch from `ports/esp32/esp_bsp/`** (`bsp_touch.c/h`)
5. **Update `bindings/micropython.cmake`** — remove `esp_lv_port/include`
6. **Update MicroPython patch**:
   - Lock file: LVGL v9.5.0 + `esp_lvgl_port` v2.7.2
   - `idf_component.yml`: updated dependencies
   - sdkconfig: remove v8-only options (`LV_COLOR_16_SWAP`, `LV_MEM_CUSTOM`, `LV_TXT_ENC_UTF8`)

See `seedsigner-c-modules/docs/lvgl-v9-migration-plan.md` for full v9 rewrite details.

## Risks & Mitigations

1. **Shell quoting of semicolons**: `MICROPY_EXTRA_COMPONENT_DIRS` passes through Make → CMake. The `\;` escape should work; fallback is two separate `-D` flags.
2. **Cross-directory REQUIRES**: `display_manager` depends on `seedsigner` in a different component dir. ESP-IDF collects all dirs before resolving — should work. Verify in Step 7.
3. **Lock file staleness**: The `dependencies.lock.esp32s3` may need regenerating. If build fails, delete lock file, let it regenerate, update patch.
4. **Submodule path change**: Changing from `sources/seedsigner-c-modules` to `deps/seedsigner-c-modules` requires `git rm` + `git submodule add` (not just a rename).
