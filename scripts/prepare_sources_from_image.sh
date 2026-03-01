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
  echo "Seeding esp-idf from prebaked image baseline..."
  cp -a /opt/toolchains/esp-idf "$WORKDIR/esp-idf"
fi

echo "Sources prepared in: $WORKDIR"
