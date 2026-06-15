#!/usr/bin/env bash
set -euo pipefail

# Verify that host-managed submodules and prebaked ESP-IDF are available.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKDIR="${1:-$ROOT_DIR/deps}"

# MicroPython is a git submodule; verify it's been initialized.
if [ ! -e "$WORKDIR/micropython/upstream/.git" ]; then
  echo "ERROR: MicroPython submodule not initialized."
  echo "Run: git submodule update --init deps/micropython/upstream"
  exit 1
fi

# seedsigner-lvgl-screens is a git submodule; verify it's been initialized.
if [ ! -e "$WORKDIR/seedsigner-lvgl-screens/.git" ]; then
  echo "ERROR: seedsigner-lvgl-screens submodule not initialized."
  echo "Run: git submodule update --init deps/seedsigner-lvgl-screens"
  exit 1
fi

# ESP-IDF is prebaked in the Docker image.
if [ -d "/opt/toolchains/esp-idf" ]; then
  echo "Using baked ESP-IDF at /opt/toolchains/esp-idf"
elif [ -d "$WORKDIR/esp-idf" ]; then
  echo "Using local ESP-IDF at $WORKDIR/esp-idf"
else
  echo "ERROR: ESP-IDF not found at /opt/toolchains/esp-idf or $WORKDIR/esp-idf"
  exit 1
fi

echo "Sources verified in: $WORKDIR"
