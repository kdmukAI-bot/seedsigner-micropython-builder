#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKDIR="${1:-$ROOT_DIR/deps}"

IDF_OVERRIDE_DIR="${IDF_OVERRIDE_DIR:-}"
PREBAKED_IDF_DIR="/opt/toolchains/esp-idf"
FALLBACK_IDF_DIR="$WORKDIR/esp-idf"

# Dependency installation is intentionally not performed here.
# Required tooling must be provided by the GHCR base image.

if [ -n "$IDF_OVERRIDE_DIR" ]; then
  IDF_DIR="$IDF_OVERRIDE_DIR"
elif [ -d "$PREBAKED_IDF_DIR" ]; then
  IDF_DIR="$PREBAKED_IDF_DIR"
else
  IDF_DIR="$FALLBACK_IDF_DIR"
fi

if [ ! -d "$IDF_DIR" ]; then
  echo "ERROR: ESP-IDF not found (checked override/prebaked/fallback)"
  exit 1
fi

if [ -z "${IDF_TOOLS_PATH:-}" ]; then
  if [ -d "/opt/espressif" ]; then
    export IDF_TOOLS_PATH="/opt/espressif"
  else
    export IDF_TOOLS_PATH="$ROOT_DIR/.espressif"
  fi
fi
mkdir -p "$IDF_TOOLS_PATH"

export IDF_PATH="$IDF_DIR"
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh"
idf.py --version >/dev/null

echo "Environment setup complete."
echo "IDF_PATH=$IDF_PATH"
echo "IDF_TOOLS_PATH=$IDF_TOOLS_PATH"
