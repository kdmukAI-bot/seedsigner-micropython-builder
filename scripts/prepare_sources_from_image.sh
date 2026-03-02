#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKDIR="${1:-$ROOT_DIR/sources}"

mkdir -p "$WORKDIR"

if [ ! -d "$WORKDIR/micropython/.git" ]; then
  echo "Seeding micropython from prebaked image baseline..."
  cp -a /opt/bases/micropython "$WORKDIR/micropython"
fi

if [ ! -d "$WORKDIR/esp-idf" ]; then
  if [ -d "/opt/toolchains/esp-idf" ]; then
    echo "Using baked ESP-IDF at /opt/toolchains/esp-idf (skipping copy to sources/)"
  else
    echo "Seeding esp-idf from prebaked image baseline..."
    cp -a /opt/toolchains/esp-idf "$WORKDIR/esp-idf"
  fi
fi


# Ensure expected upstream remote exists for baseline verification.
BASELINE_FILE="$ROOT_DIR/platform_mods/micropython_mods/BASELINE"
if [ -f "$BASELINE_FILE" ]; then
  # shellcheck disable=SC1090
  source "$BASELINE_FILE"
  UPSTREAM_REMOTE="${UPSTREAM_REMOTE:-upstream}"
  UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/micropython/micropython.git}"
  if [ -d "$WORKDIR/micropython/.git" ]; then
    if ! git -C "$WORKDIR/micropython" remote get-url "$UPSTREAM_REMOTE" >/dev/null 2>&1; then
      git -C "$WORKDIR/micropython" remote add "$UPSTREAM_REMOTE" "$UPSTREAM_URL"
      echo "Added missing remote '$UPSTREAM_REMOTE' -> $UPSTREAM_URL"
    fi
  fi
fi

echo "Sources prepared in: $WORKDIR"
