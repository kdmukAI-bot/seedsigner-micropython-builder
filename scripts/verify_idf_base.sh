#!/usr/bin/env bash
set -euo pipefail

# PLACEHOLDER: This script is not currently called. It is intended to be used by
# apply_idf_mods.sh (mirroring how apply_micropython_mods.sh calls
# verify_micropython_base.sh) once the project needs to apply patches to ESP-IDF.
#
# NOTE: The IDF_DIR path below assumes a local clone at sources/esp-idf. When
# activating this script, verify the path matches the actual IDF source location
# (currently ESP-IDF is provided by the Docker image at /opt/toolchains/esp-idf).

echo "verify_idf_base: no-op (IDF verification not enabled yet)"
exit 0

# --- Inactive verification logic below ---

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKDIR="${1:-$ROOT_DIR/sources}"
IDF_DIR="$WORKDIR/esp-idf"
BASELINE_FILE="$ROOT_DIR/platform_mods/idf_mods/BASELINE"

if [ ! -f "$BASELINE_FILE" ]; then
  echo "WARN: missing $BASELINE_FILE (using defaults)"
  UPSTREAM_REMOTE="origin"
  PATCH_BASE="origin/master"
else
  # shellcheck disable=SC1090
  source "$BASELINE_FILE"
  : "${UPSTREAM_REMOTE:=origin}"
  : "${PATCH_BASE:=origin/master}"
fi

if [ ! -e "$IDF_DIR/.git" ]; then
  echo "ERROR: expected ESP-IDF repo at: $IDF_DIR"
  exit 1
fi

cd "$IDF_DIR"
if ! git remote get-url "$UPSTREAM_REMOTE" >/dev/null 2>&1; then
  echo "ERROR: missing remote '$UPSTREAM_REMOTE' in esp-idf repo"
  exit 1
fi

git fetch "$UPSTREAM_REMOTE" --quiet
BASE_SHA="$(git rev-parse "$PATCH_BASE")"
HEAD_SHA="$(git rev-parse HEAD)"

echo "ESP-IDF repo: $(git rev-parse --show-toplevel)"
echo "HEAD: $HEAD_SHA"
echo "Baseline ($PATCH_BASE): $BASE_SHA"

if [ -n "$(git status --porcelain)" ]; then
  echo "ERROR: esp-idf working tree is dirty; clean/stash before applying mods"
  exit 1
fi

echo "OK: ESP-IDF baseline checks passed"
