#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKDIR="${1:-$ROOT_DIR/deps}"

"$SCRIPT_DIR/apply_idf_mods.sh" "$WORKDIR"
"$SCRIPT_DIR/apply_micropython_mods.sh" "$WORKDIR"
"$SCRIPT_DIR/build_firmware.sh" "$WORKDIR"
