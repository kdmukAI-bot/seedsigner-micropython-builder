#!/usr/bin/env bash
set -euo pipefail

# Reset the MicroPython submodule to its clean pinned state.
#
# This undoes any applied patches, new file overlays, and local edits,
# returning the submodule to the exact commit recorded in .gitmodules.
# Safe to run repeatedly — idempotent.
#
# Usage:
#   scripts/restore_micropython_clean.sh [DEPS_DIR]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKDIR="${1:-$ROOT_DIR/deps}"
MP_DIR="$WORKDIR/micropython/upstream"

if [ ! -e "$MP_DIR/.git" ]; then
  echo "ERROR: expected MicroPython repo at: $MP_DIR"
  exit 1
fi

# Determine the clean commit the submodule should be at.
PINNED_SHA="$(git -C "$ROOT_DIR" ls-tree HEAD deps/micropython/upstream | awk '{print $3}')"
CURRENT_SHA="$(git -C "$MP_DIR" rev-parse HEAD)"

echo "Pinned commit:  $PINNED_SHA"
echo "Current HEAD:   $CURRENT_SHA"

# Reset working tree: discard all changes and untracked files (except lib/ submodules).
git -C "$MP_DIR" checkout -- .
git -C "$MP_DIR" clean -fd --exclude=lib/

# Move HEAD to the pinned commit if needed.
if [ "$CURRENT_SHA" != "$PINNED_SHA" ]; then
  git -C "$MP_DIR" checkout "$PINNED_SHA"
  echo "Checked out pinned commit."
else
  echo "Already at pinned commit."
fi

echo "MicroPython submodule restored to clean state."
