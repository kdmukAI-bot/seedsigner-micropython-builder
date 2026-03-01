#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MODS_DIR="$ROOT_DIR/platform_mods/idf_mods"
NEW_DIR="$MODS_DIR/new_files"
PATCH_DIR="$MODS_DIR/patches"
WORKDIR="${1:-$ROOT_DIR/sources}"
IDF_DIR="$WORKDIR/esp-idf"

"$SCRIPT_DIR/verify_idf_base.sh" "$WORKDIR"

cd "$IDF_DIR"

echo "Applying IDF new file overlay from: $NEW_DIR"
rsync -a "$NEW_DIR/" "$IDF_DIR/"

echo "Applying IDF patch series from: $PATCH_DIR"
shopt -s nullglob
for p in "$PATCH_DIR"/*.patch; do
  echo "  -> $(basename "$p")"
  git apply --3way --index "$p"
done

echo "Done. Staged ESP-IDF changes:"
git status -sb
