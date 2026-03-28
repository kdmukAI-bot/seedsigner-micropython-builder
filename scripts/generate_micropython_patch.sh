#!/usr/bin/env bash
set -euo pipefail

# Generate a fresh patch set from the current MicroPython working tree.
#
# Compares the current state of the submodule against the clean pinned
# commit (v1.27.0) and writes the diff to the patches directory.  Only
# diffs files under ports/ (the ESP32 build system); lib/ submodule
# pointer changes and new_files overlay content are excluded.
#
# The generated patch is a plain unified diff that applies with
# `git apply` — no --3way needed, no blob index dependency.
#
# Prerequisites:
#   - The MicroPython submodule must have the patch applied (either via
#     apply_micropython_mods.sh or manual edits on top of it).
#   - new_files/ overlay content is NOT captured in the patch — those
#     files are managed separately in deps/micropython/mods/new_files/.
#
# Usage:
#   scripts/generate_micropython_patch.sh [DEPS_DIR]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKDIR="${1:-$ROOT_DIR/deps}"
MP_DIR="$WORKDIR/micropython/upstream"
PATCH_DIR="$ROOT_DIR/deps/micropython/mods/patches"
NEW_FILES_DIR="$ROOT_DIR/deps/micropython/mods/new_files"
PATCH_FILE="$PATCH_DIR/0001-esp32-integration-mods.patch"

if [ ! -e "$MP_DIR/.git" ]; then
  echo "ERROR: expected MicroPython repo at: $MP_DIR"
  exit 1
fi

# Determine the clean base commit.
PINNED_SHA="$(git -C "$ROOT_DIR" ls-tree HEAD deps/micropython/upstream | awk '{print $3}')"

cd "$MP_DIR"

# Build exclude list from new_files overlay.
# These files don't exist in upstream and should not be in the patch.
EXCLUDE_ARGS=()
if [ -d "$NEW_FILES_DIR" ]; then
  while IFS= read -r -d '' relpath; do
    EXCLUDE_ARGS+=(":(exclude)$relpath")
  done < <(cd "$NEW_FILES_DIR" && find . -type f -print0 | sed -z 's|^\./||')
fi

# Generate the diff: pinned commit vs current working tree (staged + unstaged).
# Only include ports/ to avoid lib/ submodule noise.
# Exclude new_files overlay paths since those are managed separately.
DIFF="$(git diff "$PINNED_SHA" -- ports/ "${EXCLUDE_ARGS[@]}")"

if [ -z "$DIFF" ]; then
  echo "No differences found between pinned commit and current tree in ports/."
  echo "Nothing to write."
  exit 0
fi

mkdir -p "$PATCH_DIR"
echo "$DIFF" > "$PATCH_FILE"

echo "Patch written to: $PATCH_FILE"
echo ""
echo "Files in patch:"
grep '^diff --git' "$PATCH_FILE" | sed 's|diff --git a/||; s| b/.*||' | sort

# Verify round-trip: restore clean, apply, check it matches.
echo ""
echo "Verifying patch applies cleanly on $PINNED_SHA..."
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
git worktree add --detach "$WORK" "$PINNED_SHA" 2>/dev/null
if git -C "$WORK" apply --check "$PATCH_FILE" 2>/dev/null; then
  echo "OK: patch applies cleanly without --3way."
else
  echo "WARNING: patch does NOT apply cleanly. Review the diff manually."
  exit 1
fi
git worktree remove "$WORK" 2>/dev/null || true
