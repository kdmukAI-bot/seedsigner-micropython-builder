#!/usr/bin/env bash
set -euo pipefail

# Expected layout under builder root:
#   <builder>/sources/micropython
#   <builder>/sources/seedsigner-c-modules
#   <builder>/sources/seedsigner-micropython-builder (this repo)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MODS_DIR="$ROOT_DIR/platform_mods/micropython_mods"
NEW_DIR="$MODS_DIR/new_files"
PATCH_DIR="$MODS_DIR/patches"

WORKDIR="${1:-$ROOT_DIR/sources}"
MP_DIR="$WORKDIR/micropython"
CMODS_DIR="$WORKDIR/seedsigner-c-modules"

"$SCRIPT_DIR/verify_micropython_base.sh" "$WORKDIR"

cd "$MP_DIR"

echo "Applying patch series from: $PATCH_DIR"
shopt -s nullglob
for p in "$PATCH_DIR"/*.patch; do
  echo "  -> $(basename "$p")"
  git apply --3way "$p"
done

echo "Applying new file overlay from: $NEW_DIR"
rsync -a "$NEW_DIR/" "$MP_DIR/"

cat > "$MP_DIR/.seedsigner-builder.env" <<ENV
SEEDSIGNER_C_MODULES_DIR=$CMODS_DIR
ENV

echo "Done. Staged changes are ready for review:"
git status -sb

echo
echo "Saved helper env file: $MP_DIR/.seedsigner-builder.env"
echo "Next: source ESP-IDF env, then build with USER_C_MODULES=$CMODS_DIR/usercmodule.cmake"
