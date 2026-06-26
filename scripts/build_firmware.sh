#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKDIR="${1:-$ROOT_DIR/deps}"
MP_DIR="$WORKDIR/micropython/upstream"
SCREENS_DIR="$WORKDIR/seedsigner-lvgl-screens"

IDF_DIR="${IDF_DIR:-}"
if [ -z "$IDF_DIR" ]; then
  if [ -d "/opt/toolchains/esp-idf" ]; then
    IDF_DIR="/opt/toolchains/esp-idf"
  else
    IDF_DIR="$WORKDIR/esp-idf"
  fi
fi

BOARD="${BOARD:-WAVESHARE_ESP32_S3_TOUCH_LCD_35B}"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/$BOARD}"
LOGS_DIR="${LOGS_DIR:-$ROOT_DIR/logs}"

if [ ! -e "$MP_DIR/.git" ]; then
  echo "ERROR: expected MicroPython repo at $MP_DIR"
  exit 1
fi
if [ ! -e "$SCREENS_DIR/.git" ]; then
  echo "ERROR: expected seedsigner-lvgl-screens repo at $SCREENS_DIR"
  exit 1
fi
if [ ! -d "$IDF_DIR" ]; then
  echo "ERROR: expected ESP-IDF at $IDF_DIR"
  exit 1
fi

mkdir -p "$BUILD_DIR" "$LOGS_DIR"
TS="$(date -u +%Y-%m-%d_%H%M%SZ)"
BUILD_LOG="$LOGS_DIR/${TS}-build-${BOARD}.log"

echo "Build log: $BUILD_LOG"

if [ -z "${IDF_TOOLS_PATH:-}" ]; then
  if [ -d "/opt/espressif" ]; then
    export IDF_TOOLS_PATH="/opt/espressif"
  else
    export IDF_TOOLS_PATH="$ROOT_DIR/.espressif"
  fi
fi

export IDF_PATH="$IDF_DIR"
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh"
idf.py --version >/dev/null 2>&1 || { echo "ERROR: idf.py not runnable (GHCR base image ESP-IDF toolchain missing/broken)"; exit 1; }

PORTS_ESP32_DIR="$ROOT_DIR/ports/esp32"
USER_C_MODULES_FILE="$ROOT_DIR/usercmodule.cmake"
MICROPY_CMAKE_ARGS="${CMAKE_ARGS:-} -DUSER_C_MODULES=$USER_C_MODULES_FILE"
MICROPY_CMAKE_ARGS="$MICROPY_CMAKE_ARGS -DMICROPY_EXTRA_COMPONENT_DIRS=${PORTS_ESP32_DIR}\;${SCREENS_DIR}/components"
MICROPY_CMAKE_ARGS="$MICROPY_CMAKE_ARGS -DSEEDSIGNER_LVGL_SCREENS_DIR=$SCREENS_DIR"

# board_common board config: maps MicroPython board name to board_common board dir.
# Override with BOARD_CONFIG_DIR env var, or auto-map from BOARD name.
BOARD_COMMON_DIR="$PORTS_ESP32_DIR/board_common"
if [ -z "${BOARD_CONFIG_DIR:-}" ]; then
  case "$BOARD" in
    WAVESHARE_ESP32_S3_TOUCH_LCD_35B) BOARD_CONFIG_DIR="$BOARD_COMMON_DIR/boards/waveshare_s3_lcd35b" ;;
    WAVESHARE_ESP32_S3_TOUCH_LCD_35)  BOARD_CONFIG_DIR="$BOARD_COMMON_DIR/boards/waveshare_s3_lcd35" ;;
    WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_35) BOARD_CONFIG_DIR="$BOARD_COMMON_DIR/boards/waveshare_p4_lcd35" ;;
    WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43) BOARD_CONFIG_DIR="$BOARD_COMMON_DIR/boards/waveshare_p4_lcd43" ;;
    *) echo "WARNING: No board_common mapping for BOARD=$BOARD"; BOARD_CONFIG_DIR="" ;;
  esac
fi
if [ -n "$BOARD_CONFIG_DIR" ] && [ -d "$BOARD_CONFIG_DIR" ]; then
  MICROPY_CMAKE_ARGS="$MICROPY_CMAKE_ARGS -DBOARD_CONFIG_DIR=$BOARD_CONFIG_DIR"
else
  echo "WARNING: BOARD_CONFIG_DIR not found: ${BOARD_CONFIG_DIR:-<unset>}"
fi

# Display height profile: maps board to the SUPPORT_DISPLAY_HEIGHT_* compile flag.
# Override with SEEDSIGNER_DISPLAY_HEIGHT env var, or auto-map from BOARD name.
if [ -z "${SEEDSIGNER_DISPLAY_HEIGHT:-}" ]; then
  case "$BOARD" in
    WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43) SEEDSIGNER_DISPLAY_HEIGHT=480 ;;
    *) SEEDSIGNER_DISPLAY_HEIGHT=320 ;;
  esac
fi
MICROPY_CMAKE_ARGS="$MICROPY_CMAKE_ARGS -DSEEDSIGNER_DISPLAY_HEIGHT=$SEEDSIGNER_DISPLAY_HEIGHT"

{
  make -C "$MP_DIR/mpy-cross" USER_C_MODULES= -j"$(nproc)"
  rm -rf "$BUILD_DIR"

  # Initialize the MicroPython submodules this board needs (e.g.
  # lib/berkeley-db-1.xx for the default MICROPY_PY_BTREE, lib/micropython-lib
  # for frozen modules). CI checks out submodules non-recursively
  # (actions/checkout submodules: true), so MicroPython's nested submodules are
  # absent and CMake config fails. MicroPython's own 'submodules' target
  # reconfigures in a throwaway dir with UPDATE_SUBMODULES=1, initializing
  # exactly the board's required set. Pass the same args as the build so the
  # reconfigure sees our components/board config. Idempotent on full clones.
  make -C "$MP_DIR/ports/esp32" \
    BOARD="$BOARD" \
    BUILD="$BUILD_DIR" \
    USER_C_MODULES="$USER_C_MODULES_FILE" \
    CMAKE_ARGS="$MICROPY_CMAKE_ARGS" \
    submodules

  # Patch fetched ESP-IDF managed components (e.g. LVGL). The `submodules`
  # reconfigure above runs the IDF component manager, which materializes
  # ports/esp32/managed_components/; patch it now, before the real build compiles
  # it. Idempotent (sentinel/dry-run guarded). See apply_component_patches.sh and
  # docs/approach-a-cache-psram-design.md.
  "$SCRIPT_DIR/apply_component_patches.sh" "$WORKDIR"

  make -C "$MP_DIR/ports/esp32" -j"$(nproc)" \
    BOARD="$BOARD" \
    BUILD="$BUILD_DIR" \
    USER_C_MODULES="$USER_C_MODULES_FILE" \
    CMAKE_ARGS="$MICROPY_CMAKE_ARGS" \
    MICROPY_MPYCROSS="$MP_DIR/mpy-cross/build/mpy-cross" \
    IDF_CCACHE_ENABLE=1

  if ! grep -Rqs "usercmodule.cmake" "$BUILD_DIR"/CMakeCache.txt "$BUILD_DIR"/esp-idf/main/CMakeFiles 2>/dev/null; then
    echo "ERROR: USER_C_MODULES not detected in build metadata (expected $USER_C_MODULES_FILE)."
    exit 1
  fi

  echo "Build complete. Artifacts:"
  ls -lh "$BUILD_DIR"/micropython.bin "$BUILD_DIR"/micropython.elf "$BUILD_DIR"/flash_args

  # Package flash-ready files for easy download/flashing.
  FLASH_DIR="$BUILD_DIR/flash"
  rm -rf "$FLASH_DIR"
  mkdir -p "$FLASH_DIR/bootloader" "$FLASH_DIR/partition_table"
  cp "$BUILD_DIR"/flash_args "$FLASH_DIR/"
  cp "$BUILD_DIR"/micropython.bin "$FLASH_DIR/"
  cp "$BUILD_DIR"/bootloader/bootloader.bin "$FLASH_DIR/bootloader/"
  cp "$BUILD_DIR"/partition_table/partition-table.bin "$FLASH_DIR/partition_table/"
  echo "Flash package: $FLASH_DIR"
  # Detect chip type from board name
  case "$BOARD" in
    *ESP32_P4*) CHIP_TYPE="esp32p4" ;;
    *)          CHIP_TYPE="esp32s3" ;;
  esac
  echo "  Flash with: python -m esptool --chip $CHIP_TYPE write_flash @flash_args"
  ls -lhR "$FLASH_DIR"
} 2>&1 | tee "$BUILD_LOG"

echo "Log saved to: $BUILD_LOG"
