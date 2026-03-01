#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKDIR="${1:-$ROOT_DIR/sources}"
CMODS_DIR="$WORKDIR/seedsigner-c-modules"

LOGS_DIR="${LOGS_DIR:-$ROOT_DIR/logs}"
TS="$(date -u +%Y-%m-%d_%H%M%SZ)"
LOG_FILE="$LOGS_DIR/${TS}-screenshots.log"

if [ ! -d "$CMODS_DIR/.git" ]; then
  echo "ERROR: expected seedsigner-c-modules repo at: $CMODS_DIR"
  exit 1
fi

mkdir -p "$LOGS_DIR"

{
  echo "Using custom modules repo: $CMODS_DIR"

  LVGL_ROOT_CANDIDATE="${LVGL_ROOT:-}"
  if [ -z "$LVGL_ROOT_CANDIDATE" ] || [ ! -d "$LVGL_ROOT_CANDIDATE" ]; then
    if [ -d "$WORKDIR/micropython/ports/esp32/managed_components/lvgl__lvgl" ]; then
      LVGL_ROOT_CANDIDATE="$WORKDIR/micropython/ports/esp32/managed_components/lvgl__lvgl"
    elif [ -d "$ROOT_DIR/build" ]; then
      LVGL_ROOT_CANDIDATE="$(find "$ROOT_DIR/build" -type d -path '*/managed_components/lvgl__lvgl' | head -n1 || true)"
    fi
  fi

  if [ -z "$LVGL_ROOT_CANDIDATE" ] || [ ! -d "$LVGL_ROOT_CANDIDATE" ]; then
    echo "ERROR: LVGL root not found. Set LVGL_ROOT or run firmware build first so managed_components/lvgl__lvgl exists."
    exit 1
  fi

  echo "Using LVGL_ROOT: $LVGL_ROOT_CANDIDATE"
  cd "$CMODS_DIR"

  cmake -S tests/screenshot_generator -B tests/screenshot_generator/build \
    -DLVGL_ROOT="$LVGL_ROOT_CANDIDATE"
  cmake --build tests/screenshot_generator/build -j"$(nproc)"
  ./tests/screenshot_generator/build/screenshot_gen

  echo "Screenshot generation complete."
  echo "Output root: $CMODS_DIR/tests/screenshot_generator/screenshots"
} 2>&1 | tee "$LOG_FILE"

echo "Log saved to: $LOG_FILE"
