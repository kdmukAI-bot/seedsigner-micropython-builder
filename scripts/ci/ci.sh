#!/usr/bin/env bash
# Shared CI script for firmware builds, screenshot generation, and deployment.
# Called by platform-specific CI configs (GitHub, GitLab, Forgejo/Codeberg).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

DEPS_DIR="$REPO_ROOT/deps"
IDF_PATH="${IDF_PATH:-/opt/toolchains/esp-idf}"
IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-/opt/espressif}"
BOARD="${BOARD:-WAVESHARE_ESP32_S3_TOUCH_LCD_35B}"

COMMAND="${1:-help}"
shift || true

case "$COMMAND" in

  # ---------------------------------------------------------------------------
  # Build steps
  # ---------------------------------------------------------------------------

  prepare-sources)
    # Mark workspace safe for git (container uid may differ from checkout).
    git config --global --add safe.directory '*' 2>/dev/null || true
    ./scripts/prepare_sources_from_image.sh "$DEPS_DIR"
    ;;

  install-idf-tools)
    test -d "$IDF_PATH"
    mkdir -p "$IDF_TOOLS_PATH"
    python3 "$IDF_PATH/tools/idf_tools.py" install riscv32-esp-elf-gdb
    # shellcheck disable=SC1091
    source "$IDF_PATH/export.sh"
    idf.py --version
    ;;

  build)
    ./scripts/ci_build.sh "$DEPS_DIR"
    ./scripts/run_screenshot_generator.sh "$DEPS_DIR"
    ;;

  record-provenance)
    mkdir -p logs
    {
      echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
      echo "builder_ref=$(git rev-parse HEAD)"
      echo "micropython_head=$(git -C deps/micropython/upstream rev-parse HEAD)"
      echo "screens_head=$(git -C deps/seedsigner-lvgl-screens rev-parse HEAD)"
      echo "idf_head=$(git -C "$IDF_PATH" rev-parse HEAD 2>/dev/null || echo unknown)"
      echo "idf_version=$(source "$IDF_PATH/export.sh" >/dev/null 2>&1; idf.py --version 2>/dev/null || echo unknown)"
      echo "python=$(python3 --version 2>&1)"
      echo "cmake=$(cmake --version | head -n1)"
    } | tee logs/provenance.txt
    ;;

  collect-artifacts)
    BUILD_DIR="$REPO_ROOT/build/$BOARD"
    mkdir -p out/bootloader out/partition_table out/logs out/screenshots
    cp "$BUILD_DIR/micropython.bin" out/
    cp "$BUILD_DIR/micropython.elf" out/
    cp "$BUILD_DIR/flash_args" out/
    cp "$BUILD_DIR/bootloader/bootloader.bin" out/bootloader/
    cp "$BUILD_DIR/partition_table/partition-table.bin" out/partition_table/
    cp -r logs/* out/logs/ 2>/dev/null || true
    cp -r "$DEPS_DIR/seedsigner-lvgl-screens/tools/screenshot_generator/screenshots/"* out/screenshots/
    ;;

  # ---------------------------------------------------------------------------
  # Frozen-app dist (MERGE-ONLY: publishes a flashable, self-booting image)
  #
  # PRs keep the fast compile-only build (no frozen_app/ -> the board manifest's
  # try/except skips the freeze). On merge to main the platform config calls
  # `stage-frozen` BEFORE `build` (so the firmware bakes the app in) and
  # `collect-dist` AFTER `collect-artifacts`.
  # ---------------------------------------------------------------------------

  stage-frozen)
    # Stage the app + embit from the pinned deps/ submodules into frozen_app/.
    # The version is computed from deps/seedsigner git and passed via env (the
    # override path in tools/_version_bake.py) so the runner never imports the
    # app -- important in CI, where version.py's own CI branch would otherwise
    # read the BUILDER's GITHUB_REF instead of the app's.
    git config --global --add safe.directory '*' 2>/dev/null || true
    # Normally already inited by the platform checkout; init defensively and
    # NON-recursively (the app's nested submodules, e.g. resources/
    # seedsigner-translations, are excluded from the freeze).
    git submodule update --init deps/seedsigner deps/embit
    SS_DIR="$REPO_ROOT/deps/seedsigner"
    export SS_APP_DIR="$SS_DIR"
    export SS_EMBIT_DIR="$REPO_ROOT/deps/embit"
    export SEEDSIGNER_VERSION="$(git -C "$SS_DIR" describe --tags --always --dirty 2>/dev/null \
      || git -C "$SS_DIR" rev-parse --short HEAD)"
    export SEEDSIGNER_SHORT_COMMIT_HASH="$(git -C "$SS_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    export SEEDSIGNER_VERSION_TIMESTAMP="$(TZ=UTC git -C "$SS_DIR" show -s \
      --date=iso-strict-local --format=%cd HEAD 2>/dev/null || date -u +%Y-%m-%dT%H:%M:%S)"
    echo "[ci] stage-frozen: version=$SEEDSIGNER_VERSION commit=$SEEDSIGNER_SHORT_COMMIT_HASH ts=$SEEDSIGNER_VERSION_TIMESTAMP"
    python3 tools/stage_frozen_app.py --board "$BOARD"
    ;;

  collect-dist)
    # Package the flashable dist/<BOARD>/ (firmware + baked /main.py launcher
    # image) and stage it under out/ for the platform's artifact upload.
    make dist BOARD="$BOARD"
    mkdir -p out/dist
    cp -r "dist/$BOARD" out/dist/
    echo "[ci] collect-dist: out/dist/$BOARD (flashable, self-booting image)"
    ;;

  # ---------------------------------------------------------------------------
  # Pages deployment (same pattern as seedsigner-lvgl-screens)
  # ---------------------------------------------------------------------------

  deploy-pages)
    # Deploy a directory to a git branch (for Pages hosting).
    # Usage: ci.sh deploy-pages SOURCE_DIR BRANCH [DEST_DIR]
    SOURCE_DIR="${1:?Usage: deploy-pages SOURCE_DIR BRANCH [DEST_DIR]}"
    PAGES_BRANCH="${2:?Usage: deploy-pages SOURCE_DIR BRANCH [DEST_DIR]}"
    DEST_DIR="${3:-.}"

    WORK=$(mktemp -d)
    trap 'rm -rf "$WORK"' EXIT

    REMOTE_URL="$(git remote get-url origin)"

    if ! git clone --depth 1 --branch "$PAGES_BRANCH" "$REMOTE_URL" "$WORK" 2>/dev/null; then
      git init "$WORK"
      git -C "$WORK" checkout --orphan "$PAGES_BRANCH"
      git -C "$WORK" remote add origin "$REMOTE_URL"
    fi

    if [ "$DEST_DIR" = "." ]; then
      find "$WORK" -maxdepth 1 ! -name '.git' ! -name '.' -exec rm -rf {} +
      cp -r "$SOURCE_DIR"/. "$WORK/"
    else
      rm -rf "$WORK/$DEST_DIR"
      mkdir -p "$WORK/$DEST_DIR"
      cp -r "$SOURCE_DIR"/. "$WORK/$DEST_DIR/"
    fi

    git -C "$WORK" add -A
    if git -C "$WORK" diff --cached --quiet; then
      echo "No changes to deploy."
    else
      git -C "$WORK" \
        -c user.name="ci-bot" \
        -c user.email="ci-bot@noreply" \
        commit -m "Deploy screenshots"
      git -C "$WORK" push origin "$PAGES_BRANCH"
    fi
    ;;

  cleanup-pages)
    # Remove a subdirectory from the pages branch.
    # Usage: ci.sh cleanup-pages BRANCH SUBDIR
    PAGES_BRANCH="${1:?Usage: cleanup-pages BRANCH SUBDIR}"
    SUBDIR="${2:?Usage: cleanup-pages BRANCH SUBDIR}"

    WORK=$(mktemp -d)
    trap 'rm -rf "$WORK"' EXIT

    REMOTE_URL="$(git remote get-url origin)"
    if ! git clone --depth 1 --branch "$PAGES_BRANCH" "$REMOTE_URL" "$WORK" 2>/dev/null; then
      echo "Pages branch '$PAGES_BRANCH' does not exist; nothing to clean."
      exit 0
    fi

    if [ -d "$WORK/$SUBDIR" ]; then
      rm -rf "$WORK/$SUBDIR"
      git -C "$WORK" add -A
      git -C "$WORK" \
        -c user.name="ci-bot" \
        -c user.email="ci-bot@noreply" \
        commit -m "Remove $SUBDIR"
      git -C "$WORK" push origin "$PAGES_BRANCH"
    else
      echo "Directory '$SUBDIR' not found on '$PAGES_BRANCH'; nothing to clean."
    fi
    ;;

  # ---------------------------------------------------------------------------
  # Help
  # ---------------------------------------------------------------------------

  help|*)
    echo "Usage: $0 COMMAND [ARGS...]"
    echo ""
    echo "Build commands:"
    echo "  prepare-sources          Verify submodules and prebaked ESP-IDF"
    echo "  install-idf-tools        Install additional IDF tools (riscv32 debugger)"
    echo "  build                    Build firmware + screenshots"
    echo "  record-provenance        Write build metadata to logs/provenance.txt"
    echo "  collect-artifacts        Gather firmware + screenshots into out/"
    echo ""
    echo "Frozen-app dist commands (merge-only):"
    echo "  stage-frozen             Stage app+embit from deps/ submodules -> frozen_app/ (run BEFORE build)"
    echo "  collect-dist             make dist + copy flashable dist/<BOARD>/ into out/ (run AFTER build)"
    echo ""
    echo "Pages commands:"
    echo "  deploy-pages SRC BRANCH [DEST]   Deploy directory to a git branch"
    echo "  cleanup-pages BRANCH SUBDIR      Remove a subdirectory from pages branch"
    exit 1
    ;;
esac
