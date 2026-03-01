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
  cd "$CMODS_DIR"

  cmake -S tests/screenshot_generator -B tests/screenshot_generator/build
  cmake --build tests/screenshot_generator/build -j"$(nproc)"
  ./tests/screenshot_generator/build/screenshot_gen

  echo "Screenshot generation complete."
  echo "Output root: $CMODS_DIR/tests/screenshot_generator/screenshots"
} 2>&1 | tee "$LOG_FILE"

echo "Log saved to: $LOG_FILE"
