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

mkdir -p "$BUILD_DIR" "$LOGS_DIR"
TS="$(date -u +%Y-%m-%d_%H%M%SZ)"
BUILD_LOG="$LOGS_DIR/${TS}-build-${BOARD}.log"

echo "Build log: $BUILD_LOG"
if [ ! -d "$IDF_DIR" ]; then
  echo "ERROR: expected ESP-IDF at $IDF_DIR"
  exit 1
fi

export IDF_PATH="$IDF_DIR"
export IDF_TOOLS_TARGETS="${IDF_TOOLS_TARGETS:-esp32s3}"

MICROPY_CMAKE_ARGS="${CMAKE_ARGS:-}"
if [ -d "$CMODS_DIR/components" ]; then
  MICROPY_CMAKE_ARGS="${MICROPY_CMAKE_ARGS} -DMICROPY_EXTRA_COMPONENT_DIRS=$CMODS_DIR/components"
fi
echo "Using CMAKE_ARGS: $MICROPY_CMAKE_ARGS"

if [ -z "${IDF_TOOLS_PATH:-}" ]; then
  if [ -d "/opt/espressif" ]; then
    export IDF_TOOLS_PATH="/opt/espressif"
  else
    export IDF_TOOLS_PATH="$ROOT_DIR/.espressif"
  fi
fi

# shellcheck disable=SC1091
if ! source "$IDF_PATH/export.sh" >/dev/null 2>&1; then
  echo "ERROR: failed to source ESP-IDF export script: $IDF_PATH/export.sh"
  echo "Hint: run ./scripts/setup_env.sh (host) or use the prebaked base image workflow."
  exit 1
fi

if ! idf.py --version >/dev/null 2>&1; then
  echo "ERROR: idf.py is not runnable after sourcing ESP-IDF environment"
  echo "IDF_TOOLS_PATH=$IDF_TOOLS_PATH"
  echo "Hint: run ./scripts/setup_env.sh to install ESP-IDF tools for this workspace."
  exit 1
fi

# build mpy-cross from canonical tree
{
  make -C "$MP_DIR/mpy-cross" USER_C_MODULES= -j"$(nproc)"

  # clean board build dir to avoid stale path/cmake cache issues
  rm -rf "$BUILD_DIR"

  make -C "$MP_DIR/ports/esp32" -j"$(nproc)" \
    BOARD="$BOARD" \
    BUILD="$BUILD_DIR" \
    USER_C_MODULES="$CMODS_DIR/usercmodule.cmake" \
    CMAKE_ARGS="$MICROPY_CMAKE_ARGS" \
    MICROPY_MPYCROSS="$MP_DIR/mpy-cross/build/mpy-cross" \
    IDF_CCACHE_ENABLE=1

  echo "Build complete. Artifacts:"
  ls -lh "$BUILD_DIR"/micropython.bin "$BUILD_DIR"/micropython.elf "$BUILD_DIR"/flash_args
} 2>&1 | tee "$BUILD_LOG"

echo "Log saved to: $BUILD_LOG"
