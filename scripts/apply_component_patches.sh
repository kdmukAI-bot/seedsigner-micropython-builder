#!/usr/bin/env bash
set -euo pipefail

# Apply hand-maintained patches to fetched ESP-IDF *managed components* (e.g. LVGL).
#
# Why this exists: the normal source-patch flow (apply_micropython_mods.sh) only
# diffs/patches ports/ and runs BEFORE the IDF component manager materializes
# deps/.../managed_components/. Managed components therefore can't be reached that
# way. This script applies deps/micropython/mods/component_patches/*.patch to the
# fetched component tree instead. See docs/approach-a-cache-psram-design.md.
#
# Ordering: must run AFTER the component manager has fetched managed_components/
# (the `make ... submodules` reconfigure in build_firmware.sh) and BEFORE the real
# build compiles LVGL.
#
# Idempotent: uses `patch --dry-run` (forward) and `patch --reverse --dry-run`
# (already-applied) to decide, so re-runs are safe and a freshly re-fetched
# (pristine) component is re-patched. A missing component with pending patches is
# a hard error -- we never silently ship an unpatched build.
#
# Convention: any patch named `lvgl-*.patch` targets the lvgl__lvgl component and
# is applied with `patch -p1` from that component's root (the diffs use a/ b/
# prefixes rooted at the component, e.g. a/src/misc/lv_rb.c).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKDIR="${1:-$ROOT_DIR/deps}"
MP_DIR="$WORKDIR/micropython/upstream"
PATCH_DIR="$ROOT_DIR/deps/micropython/mods/component_patches"
COMP_ROOT="$MP_DIR/ports/esp32/managed_components"

if [ ! -d "$PATCH_DIR" ] || ! ls "$PATCH_DIR"/*.patch >/dev/null 2>&1; then
  echo "apply_component_patches: no component patches to apply"
  exit 0
fi

apply_one() {
  # $1 = component root dir, $2 = patch file
  local comp_dir="$1" patch_file="$2" name
  name="$(basename "$patch_file")"

  if [ ! -d "$comp_dir" ]; then
    echo "ERROR: component dir missing for $name: $comp_dir"
    echo "       (managed component not fetched yet -- run after the component manager"
    echo "        has materialized managed_components/, e.g. after 'make ... submodules')"
    exit 1
  fi

  if patch -p1 --dry-run --silent -d "$comp_dir" < "$patch_file" >/dev/null 2>&1; then
    patch -p1 --silent -d "$comp_dir" < "$patch_file"
    echo "apply_component_patches: applied $name"
  elif patch -p1 --reverse --dry-run --silent -d "$comp_dir" < "$patch_file" >/dev/null 2>&1; then
    echo "apply_component_patches: already applied, skipping $name"
  else
    echo "ERROR: $name neither applies cleanly nor is already applied to $comp_dir"
    echo "       The managed component may have drifted from the patch baseline"
    echo "       (LVGL version bump?). Re-base the patch. See docs/approach-a-cache-psram-design.md."
    exit 1
  fi
}

shopt -s nullglob
for p in "$PATCH_DIR"/*.patch; do
  case "$(basename "$p")" in
    lvgl-*) apply_one "$COMP_ROOT/lvgl__lvgl" "$p" ;;
    *)
      echo "ERROR: no component mapping for patch $(basename "$p") (expected lvgl-*.patch)"
      exit 1
      ;;
  esac
done

echo "apply_component_patches: done"
