#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

# Ensure writable HOME/cache for non-root container users.
export HOME="/tmp/home"
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$HOME/.cache}"
mkdir -p "$HOME" "$XDG_CACHE_HOME"

# Avoid git ownership issues with host-mounted workspace and prebaked IDF checkout.
git config --global --add safe.directory '*' || true
git config --global --add safe.directory /opt/toolchains/esp-idf || true

mkdir -p deps
./scripts/prepare_sources_from_image.sh "$ROOT_DIR/deps"

if [ ! -e "$ROOT_DIR/deps/seedsigner-c-modules/.git" ]; then
  git clone https://github.com/kdmukAI-bot/seedsigner-c-modules.git "$ROOT_DIR/deps/seedsigner-c-modules"
fi

./scripts/setup_env.sh "$ROOT_DIR/deps"
./scripts/ci_build.sh "$ROOT_DIR/deps"
./scripts/run_screenshot_generator.sh "$ROOT_DIR/deps"

echo "DONE: firmware + screenshots built."
