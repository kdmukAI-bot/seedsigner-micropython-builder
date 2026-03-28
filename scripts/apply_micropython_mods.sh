#!/usr/bin/env bash
set -euo pipefail

# Expected layout under builder root:
#   <builder>/deps/micropython/upstream   (git submodule)
#   <builder>/deps/seedsigner-c-modules   (git submodule)
#   <builder>/deps/micropython/mods/

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MODS_DIR="$ROOT_DIR/deps/micropython/mods"
NEW_DIR="$MODS_DIR/new_files"
PATCH_DIR="$MODS_DIR/patches"

WORKDIR="${1:-$ROOT_DIR/deps}"
MP_DIR="$WORKDIR/micropython/upstream"
CMODS_DIR="$WORKDIR/seedsigner-c-modules"

if [ ! -e "$MP_DIR/.git" ]; then
  echo "ERROR: expected MicroPython repo at: $MP_DIR"
  exit 1
fi

# Developer mode: if MicroPython tree has uncommitted changes beyond
# the seedsigner patch commit, leave it as-is for iterative development.
if [ -n "$(git -C "$MP_DIR" status --porcelain --ignore-submodules)" ]; then
  echo "MicroPython tree is dirty; skipping patch apply and using current working tree."
  exit 0
fi

# Check if the seedsigner patch has already been applied (committed).
if git -C "$MP_DIR" log --oneline -1 | grep -q "seedsigner-builder: applied patch"; then
  echo "Seedsigner patch already applied; skipping."
  exit 0
fi

"$SCRIPT_DIR/verify_micropython_base.sh" "$WORKDIR"

cd "$MP_DIR"

echo "Applying patch series from: $PATCH_DIR"
shopt -s nullglob
for p in "$PATCH_DIR"/*.patch; do
  echo "  -> $(basename "$p")"
  git apply "$p"
done

echo "Applying new file overlay from: $NEW_DIR"
rsync -a "$NEW_DIR/" "$MP_DIR/"

# Commit the patch result so developer changes are cleanly separated.
# `git diff` will show only dev changes; `git diff HEAD~1` shows the full patch.
git add -A -- ':!lib/'
git -c user.name="seedsigner-builder" -c user.email="builder@localhost" \
  commit -m "seedsigner-builder: applied patch series + board overlay" --no-gpg-sign

echo "Done. Patch committed in submodule:"
git log --oneline -1

echo
echo "To iterate: edit files, rebuild. Your changes will be on top of this commit."
echo "To regenerate patch: scripts/generate_micropython_patch.sh"
echo "To restore clean state: scripts/restore_micropython_clean.sh"
