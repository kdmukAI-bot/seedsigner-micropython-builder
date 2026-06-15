#!/usr/bin/env bash
set -euo pipefail

# Verify that MicroPython submodule is at the expected baseline commit
# and that the working tree is clean before applying patches.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

WORKDIR="${1:-$ROOT_DIR/deps}"
MP_DIR="$WORKDIR/micropython/upstream"
SCREENS_DIR="$WORKDIR/seedsigner-lvgl-screens"

if [ ! -e "$MP_DIR/.git" ]; then
  echo "ERROR: expected MicroPython repo at: $MP_DIR"
  exit 1
fi

if [ ! -e "$SCREENS_DIR/.git" ]; then
  echo "ERROR: expected seedsigner-lvgl-screens repo at: $SCREENS_DIR"
  exit 1
fi

cd "$MP_DIR"
HEAD_SHA="$(git rev-parse HEAD)"

echo "Builder root: $ROOT_DIR"
echo "Sources: $WORKDIR"
echo "MicroPython repo: $(git rev-parse --show-toplevel)"
echo "Custom modules repo: $SCREENS_DIR"
echo "HEAD: $HEAD_SHA"

if [ -n "$(git status --porcelain --ignore-submodules)" ]; then
  echo "ERROR: micropython working tree is dirty; clean/stash before applying mods"
  exit 1
fi

echo "OK: workspace layout and micropython baseline checks passed"
