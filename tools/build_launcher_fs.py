#!/usr/bin/env python3
"""Build the baked-launcher littlefs2 image (`vfs.bin`) for a flashable dist.

A FROZEN firmware freezes the whole app but has NO launcher of its own, so a fresh
flash boots to the REPL until something writes `/main.py` (see
docs/knowledge/micropython-frozen-vs-vfs-override.md). To publish a *self-booting*
image, we bake `/main.py` into a littlefs2 filesystem image and flash it into the
board's internal "vfs" partition alongside the firmware.

That partition is NOT declared in the CSV: MicroPython's esp32 port auto-registers a
"vfs" partition at runtime (`ports/esp32/main.c:251`) spanning from the end of the
last table partition to the end of flash, formatted littlefs2. So its geometry is
fully determined by build artifacts:
  * offset = max(partition.address + partition.size) over the built partition table
  * size   = flash_size - offset      (flash_size from the built flash_args)
  * block_size = 4096, block_count = size / 4096

The image is produced by tools/mklittlefs_launcher.c, compiled against MicroPython's
OWN vendored littlefs2 so the on-disk format matches the firmware exactly.

Usage (dist integration — derive geometry from a build, emit vfs.bin, extend flash_args):
    python3 tools/build_launcher_fs.py --board <BOARD> \
        --build-dir build/<BOARD> --dist-dir dist/<BOARD>

Usage (standalone / testing — explicit geometry):
    python3 tools/build_launcher_fs.py --offset 0xC50000 --flash-size 32MB --out /tmp/vfs.bin
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _launcher import MAIN_PY  # noqa: E402  (the single launcher definition all writers share)

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
LFS_DIR = os.path.join(REPO_ROOT, "deps", "micropython", "upstream", "lib", "littlefs")
MKLFS_SRC = os.path.join(REPO_ROOT, "tools", "mklittlefs_launcher.c")
BLOCK_SIZE = 4096          # esp32 Partition NATIVE_BLOCK_SIZE_BYTES (littlefs block_size)
LAUNCHER_NAME = "main.py"  # littlefs root-relative; boot runs cwd-relative "main.py" at "/"

# esp-idf partition-table entry: <2s magic 0x50AA><B type><B subtype><I offset><I size>
# <16s label><I flags> = 32 bytes. The table ends at the first non-0x50AA entry
# (e.g. the 0xEBEB md5 row).
PART_MAGIC = b"\xaa\x50"
PART_ENTRY = 32


def _die(msg):
    sys.exit("[build-launcher-fs] ERROR: " + msg)


def parse_flash_size(flash_args_path):
    """Read the flash size (bytes) from a built flash_args (`--flash_size 32MB`)."""
    with open(flash_args_path) as f:
        text = f.read()
    m = re.search(r"--flash_size\s+(\d+)(MB|KB)?", text)
    if not m:
        _die("no --flash_size in %s" % flash_args_path)
    n = int(m.group(1))
    unit = m.group(2) or "MB"
    return n * (1024 * 1024 if unit == "MB" else 1024)


def parse_size_arg(s):
    """Parse an explicit --flash-size (e.g. '32MB', '0x2000000', '33554432')."""
    s = s.strip()
    m = re.match(r"^(\d+)(MB|KB)$", s)
    if m:
        return int(m.group(1)) * (1024 * 1024 if m.group(2) == "MB" else 1024)
    return int(s, 0)


def vfs_offset_from_table(part_bin_path):
    """Compute the auto-vfs offset = max(offset+size) over the built partition table
    — exactly what ports/esp32/main.c uses to place the runtime 'vfs' partition."""
    with open(part_bin_path, "rb") as f:
        data = f.read()
    end = 0
    seen = False
    for i in range(0, len(data) - PART_ENTRY + 1, PART_ENTRY):
        if data[i:i + 2] != PART_MAGIC:
            break  # end of real entries (md5 row / padding)
        offset = int.from_bytes(data[i + 4:i + 8], "little")
        size = int.from_bytes(data[i + 8:i + 12], "little")
        end = max(end, offset + size)
        seen = True
    if not seen:
        _die("no partition entries found in %s" % part_bin_path)
    return end


def compile_tool():
    """Compile mklittlefs_launcher against the vendored littlefs2. Fast (~1s); built
    fresh into a temp path so there's no stale-binary risk in CI."""
    lfs2_c = os.path.join(LFS_DIR, "lfs2.c")
    lfs2_util_c = os.path.join(LFS_DIR, "lfs2_util.c")
    for p in (MKLFS_SRC, lfs2_c, lfs2_util_c):
        if not os.path.exists(p):
            _die("missing source for the littlefs image tool: %s" % p)
    out = os.path.join(tempfile.gettempdir(), "mklittlefs_launcher")
    cc = os.environ.get("CC", "gcc")
    cmd = [cc, "-O2", "-o", out, MKLFS_SRC, lfs2_c, lfs2_util_c, "-I", LFS_DIR]
    try:
        subprocess.run(cmd, check=True)
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        _die("failed to compile mklittlefs_launcher (%s).\n"
             "       need a C compiler (set $CC); cmd: %s" % (e, " ".join(cmd)))
    return out


def build_image(out_path, offset, flash_size):
    if flash_size <= offset:
        _die("flash size 0x%x <= vfs offset 0x%x — no room for a vfs partition"
             % (flash_size, offset))
    size = flash_size - offset
    if size % BLOCK_SIZE:
        _die("vfs size 0x%x not a multiple of block size %d" % (size, BLOCK_SIZE))
    block_count = size // BLOCK_SIZE

    tool = compile_tool()
    # Write the launcher content to a temp file for the C tool to embed as /main.py.
    fd, main_py = tempfile.mkstemp(prefix="main_py_", suffix=".txt")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(MAIN_PY.encode("utf-8"))
        cmd = [tool, out_path, str(BLOCK_SIZE), str(block_count), main_py, LAUNCHER_NAME]
        subprocess.run(cmd, check=True)
    finally:
        os.remove(main_py)
    print("[build-launcher-fs] vfs image: %s  (offset 0x%x, vfs size 0x%x = %d blocks, flash 0x%x)"
          % (out_path, offset, size, block_count, flash_size))
    return offset


def append_flash_args(flash_args_path, offset, image_basename):
    """Add `<offset> <image>` to a dist flash_args (idempotent)."""
    with open(flash_args_path) as f:
        lines = f.read().splitlines()
    line = "0x%x %s" % (offset, image_basename)
    # Replace any existing entry for this image (avoid dupes / stale offset), else append.
    kept = [ln for ln in lines if ln.split()[1:2] != [image_basename]]
    kept.append(line)
    with open(flash_args_path, "w") as f:
        f.write("\n".join(kept) + "\n")
    print("[build-launcher-fs] flash_args += '%s'  (%s)" % (line, flash_args_path))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", help="board name (for messages / default dirs)")
    ap.add_argument("--build-dir", help="build/<BOARD> — source of partition-table.bin + flash_args")
    ap.add_argument("--dist-dir", help="dist/<BOARD> — where vfs.bin is written and flash_args extended")
    ap.add_argument("--out", help="output image path (default: <dist-dir>/vfs.bin)")
    ap.add_argument("--offset", help="explicit vfs offset (hex/dec) — overrides build-dir derivation")
    ap.add_argument("--flash-size", help="explicit flash size (e.g. 32MB / 0x2000000) — overrides derivation")
    ap.add_argument("--no-flash-args", action="store_true",
                    help="only emit the image; do not touch flash_args")
    args = ap.parse_args()

    # Geometry: explicit args win; otherwise derive from the build artifacts.
    if args.offset is not None:
        offset = int(args.offset, 0)
    elif args.build_dir:
        offset = vfs_offset_from_table(os.path.join(args.build_dir, "partition_table",
                                                    "partition-table.bin"))
    else:
        _die("need --offset or --build-dir to determine the vfs offset")

    if args.flash_size is not None:
        flash_size = parse_size_arg(args.flash_size)
    elif args.build_dir:
        flash_size = parse_flash_size(os.path.join(args.build_dir, "flash_args"))
    else:
        _die("need --flash-size or --build-dir to determine the flash size")

    out_path = args.out
    if not out_path:
        if not args.dist_dir:
            _die("need --out or --dist-dir for the output image path")
        out_path = os.path.join(args.dist_dir, "vfs.bin")
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)

    build_image(out_path, offset, flash_size)

    if not args.no_flash_args and args.dist_dir:
        append_flash_args(os.path.join(args.dist_dir, "flash_args"), offset,
                          os.path.basename(out_path))


if __name__ == "__main__":
    main()
