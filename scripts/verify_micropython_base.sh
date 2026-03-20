#!/usr/bin/env bash
set -euo pipefail

# Expected layout under builder root:
#   <builder>/deps/micropython/upstream
#   <builder>/deps/seedsigner-c-modules
#   <builder>/deps/micropython/mods/

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BASELINE_FILE="$ROOT_DIR/deps/micropython/mods/BASELINE"

WORKDIR="${1:-$ROOT_DIR/deps}"
MP_DIR="$WORKDIR/micropython/upstream"
CMODS_DIR="$WORKDIR/seedsigner-c-modules"

if [ ! -e "$MP_DIR/.git" ]; then
  echo "ERROR: expected MicroPython repo at: $MP_DIR"
  exit 1
fi

if [ ! -e "$CMODS_DIR/.git" ]; then
  echo "ERROR: expected seedsigner-c-modules repo at: $CMODS_DIR"
  exit 1
fi

# shellcheck disable=SC1090
source "$BASELINE_FILE"

cd "$MP_DIR"

if ! git remote get-url "$UPSTREAM_REMOTE" >/dev/null 2>&1; then
  echo "ERROR: missing remote '$UPSTREAM_REMOTE' in micropython repo"
  exit 1
fi

git fetch "$UPSTREAM_REMOTE" --quiet
BASE_SHA="$(git rev-parse "$PATCH_BASE")"
HEAD_SHA="$(git rev-parse HEAD)"

echo "Builder root: $ROOT_DIR"
echo "Sources: $WORKDIR"
echo "MicroPython repo: $(git rev-parse --show-toplevel)"
echo "Custom modules repo: $CMODS_DIR"
echo "HEAD: $HEAD_SHA"
echo "Baseline ($PATCH_BASE): $BASE_SHA"

if [ -n "$(git status --porcelain)" ]; then
  echo "ERROR: micropython working tree is dirty; clean/stash before applying mods"
  exit 1
fi

echo "OK: workspace layout and micropython baseline checks passed"
