#!/usr/bin/env bash
set -euo pipefail

# Expected layout under builder root:
#   <builder>/sources/micropython
#   <builder>/sources/seedsigner-c-modules
#   <builder>/sources/seedsigner-micropython-builder (this repo)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BASELINE_FILE="$ROOT_DIR/platform_mods/micropython_mods/BASELINE"

WORKDIR="${1:-$ROOT_DIR/sources}"
MP_DIR="$WORKDIR/micropython"
CMODS_DIR="$WORKDIR/seedsigner-c-modules"

if [ ! -d "$MP_DIR/.git" ]; then
  echo "ERROR: expected MicroPython repo at: $MP_DIR"
  exit 1
fi

if [ ! -d "$CMODS_DIR/.git" ]; then
  echo "ERROR: expected seedsigner-c-modules repo at: $CMODS_DIR"
  exit 1
fi

# shellcheck disable=SC1090
source "$BASELINE_FILE"

cd "$MP_DIR"

FETCH_REMOTE="$UPSTREAM_REMOTE"
if ! git remote get-url "$FETCH_REMOTE" >/dev/null 2>&1; then
  if git remote get-url origin >/dev/null 2>&1; then
    echo "WARN: missing remote '$UPSTREAM_REMOTE'; falling back to 'origin'"
    FETCH_REMOTE=origin
  else
    echo "ERROR: missing remotes '$UPSTREAM_REMOTE' and 'origin' in micropython repo"
    exit 1
  fi
fi

PATCH_BASE_REF="$PATCH_BASE"
if [[ "$PATCH_BASE_REF" == "$UPSTREAM_REMOTE/"* ]]; then
  PATCH_BASE_REF="$FETCH_REMOTE/${PATCH_BASE_REF#${UPSTREAM_REMOTE}/}"
fi

git fetch "$FETCH_REMOTE" --quiet
BASE_SHA="$(git rev-parse "$PATCH_BASE_REF")"
HEAD_SHA="$(git rev-parse HEAD)"

echo "Builder root: $ROOT_DIR"
echo "Sources: $WORKDIR"
echo "MicroPython repo: $(git rev-parse --show-toplevel)"
echo "Custom modules repo: $CMODS_DIR"
echo "HEAD: $HEAD_SHA"
echo "Baseline ($PATCH_BASE_REF): $BASE_SHA"

if [ -n "$(git status --porcelain)" ]; then
  echo "ERROR: micropython working tree is dirty; clean/stash before applying mods"
  exit 1
fi

echo "OK: workspace layout and micropython baseline checks passed"
