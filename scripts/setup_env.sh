#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKDIR="${1:-$ROOT_DIR/sources}"
IDF_DIR="$WORKDIR/esp-idf"

# Host deps (Ubuntu/Debian)
if command -v apt-get >/dev/null 2>&1; then
  sudo apt-get update
  sudo apt-get install -y \
    git make cmake ninja-build ccache rsync \
    python3 python3-venv python3-pip \
    libffi-dev libssl-dev libpng-dev \
    dfu-util libusb-1.0-0 libsdl2-2.0-0 libslirp0
fi

if [ ! -d "$IDF_DIR" ]; then
  echo "ERROR: ESP-IDF not found at $IDF_DIR"
  echo "Clone it first under sources/, e.g.:"
  echo "  git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git $IDF_DIR"
  exit 1
fi

if [ -z "${IDF_TOOLS_PATH:-}" ]; then
  if [ -d "/opt/espressif" ]; then
    export IDF_TOOLS_PATH="/opt/espressif"
  else
    export IDF_TOOLS_PATH="$ROOT_DIR/.espressif"
  fi
fi

export IDF_TOOLS_TARGETS="${IDF_TOOLS_TARGETS:-esp32s3}"

mkdir -p "$IDF_TOOLS_PATH"

cd "$IDF_DIR"
./install.sh esp32s3
# shellcheck disable=SC1091
source "$IDF_DIR/export.sh"
python tools/idf_tools.py check || python tools/idf_tools.py install

echo "Environment setup complete."
