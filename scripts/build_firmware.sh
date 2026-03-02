#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKDIR="${1:-$ROOT_DIR/sources}"
MP_DIR="$WORKDIR/micropython"
CMODS_DIR="$WORKDIR/seedsigner-c-modules"

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

if [ ! -d "$MP_DIR/.git" ]; then
  echo "ERROR: expected MicroPython repo at $MP_DIR"
  exit 1
fi
if [ ! -d "$CMODS_DIR/.git" ]; then
  echo "ERROR: expected seedsigner-c-modules repo at $CMODS_DIR"
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
python3 "$IDF_PATH/tools/idf_tools.py" install riscv32-esp-elf-gdb >/dev/null 2>&1 || true
idf.py --version >/dev/null 2>&1 || { echo "ERROR: idf.py not runnable"; exit 1; }

USER_C_MODULES_FILE="$CMODS_DIR/usercmodule.cmake"
MICROPY_CMAKE_ARGS="${CMAKE_ARGS:-} -DUSER_C_MODULES=$USER_C_MODULES_FILE"
if [ -d "$CMODS_DIR/components" ]; then
  MICROPY_CMAKE_ARGS="$MICROPY_CMAKE_ARGS -DMICROPY_EXTRA_COMPONENT_DIRS=$CMODS_DIR/components"
fi

{
  make -C "$MP_DIR/mpy-cross" USER_C_MODULES= -j"$(nproc)"
  rm -rf "$BUILD_DIR"
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
} 2>&1 | tee "$BUILD_LOG"

echo "Log saved to: $BUILD_LOG"
