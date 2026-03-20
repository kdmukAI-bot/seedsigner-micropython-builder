#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKDIR="${1:-$ROOT_DIR/deps}"

mkdir -p "$WORKDIR/micropython"

if [ ! -e "$WORKDIR/micropython/upstream/.git" ]; then
  echo "Seeding micropython from prebaked image baseline..."
  cp -a /opt/bases/micropython "$WORKDIR/micropython/upstream"
fi

if [ -d "/opt/toolchains/esp-idf" ]; then
  echo "Using baked ESP-IDF at /opt/toolchains/esp-idf"
elif [ ! -d "$WORKDIR/esp-idf" ]; then
  echo "ERROR: no baked ESP-IDF found at /opt/toolchains/esp-idf and no fallback at $WORKDIR/esp-idf"
  exit 1
fi

BASELINE_FILE="$ROOT_DIR/deps/micropython/mods/BASELINE"
if [ -f "$BASELINE_FILE" ]; then
  # shellcheck disable=SC1090
  source "$BASELINE_FILE"
  UPSTREAM_REMOTE="${UPSTREAM_REMOTE:-upstream}"
  UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/micropython/micropython.git}"
  if [ -e "$WORKDIR/micropython/upstream/.git" ]; then
    if ! git -C "$WORKDIR/micropython/upstream" remote get-url "$UPSTREAM_REMOTE" >/dev/null 2>&1; then
      git -C "$WORKDIR/micropython/upstream" remote add "$UPSTREAM_REMOTE" "$UPSTREAM_URL"
      echo "Added missing remote '$UPSTREAM_REMOTE' -> $UPSTREAM_URL"
    fi
  fi
fi

echo "Sources prepared in: $WORKDIR"
