#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

MP_DIR="$ROOT_DIR/deps/micropython/upstream"

# Check if the MicroPython tree is already dirty (patched or edited).
# A dirty tree on entry means the developer is iterating on changes.
# This requires MP_ALLOW_DIRTY=1 to proceed — without it, fail early
# so accidental builds on a stale/unknown tree don't go unnoticed.
MP_DIRTY=false
if [ -e "$MP_DIR/.git" ]; then
  if [ -n "$(git -C "$MP_DIR" status --porcelain --ignore-submodules 2>/dev/null)" ] || \
     git -C "$MP_DIR" log --oneline -1 2>/dev/null | grep -q "seedsigner-builder: applied patch"; then
    MP_DIRTY=true
  fi
fi

if [ "$MP_DIRTY" = true ] && [ "${MP_ALLOW_DIRTY:-}" != "1" ]; then
  echo "ERROR: MicroPython submodule has uncommitted changes or applied patches."
  echo ""
  echo "If you are intentionally iterating on MicroPython changes, re-run with:"
  echo "  MP_ALLOW_DIRTY=1 make docker-build-all"
  echo ""
  echo "To restore to a clean state first:"
  echo "  scripts/restore_micropython_clean.sh"
  exit 1
fi

# If the tree is clean, restore on exit after the build applies patches.
# If dirty (developer mode), leave the tree as-is.
cleanup() {
  if [ "$MP_DIRTY" = true ]; then
    echo "Skipping cleanup: MicroPython tree was already patched/dirty before build."
    echo "To restore clean state: scripts/restore_micropython_clean.sh"
  else
    ./scripts/restore_micropython_clean.sh "$ROOT_DIR/deps" 2>/dev/null || true
  fi
}
trap cleanup EXIT

# Avoid git ownership issues with host-mounted workspace and prebaked IDF checkout.
git config --global --add safe.directory '*' 2>/dev/null || true

./scripts/prepare_sources_from_image.sh "$ROOT_DIR/deps"
./scripts/setup_env.sh "$ROOT_DIR/deps"
./scripts/ci_build.sh "$ROOT_DIR/deps"
./scripts/run_screenshot_generator.sh "$ROOT_DIR/deps"

echo "DONE: firmware + screenshots built."
