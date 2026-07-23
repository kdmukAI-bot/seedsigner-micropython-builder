# ESP32 auto-vfs partition + baking the frozen-app launcher into a flashable image

How the SeedSigner ESP32 firmware gets a persistent internal filesystem despite the
partition table declaring none, and how `make dist` bakes the `/main.py` launcher into
that filesystem so a freshly flashed image boots the app instead of the REPL. Verified
against MicroPython v1.27.0 as built here, on the ESP32-P4-43 (32 MB).

## There is no `vfs` partition in the CSV — the port creates one at runtime

The board partition CSVs
(`deps/micropython/mods/new_files/ports/esp32/partitions-{16,32}MiB-waveshare.csv`)
declare only `nvs / phy_init / factory / coredump`. There is **no** `vfs`/`ffat` data
partition. Yet `/` mounts and persists (holding `/main.py`, `/boot.py`, `/lib`, `/overlay`).

The reason: MicroPython's esp32 port **auto-registers a `vfs` partition at runtime**
(`deps/micropython/upstream/ports/esp32/main.c:251`). When no `vfs`/`ffat` partition
exists, it registers one spanning **from the end of the last table partition to the end
of flash**:

- offset = `max(partition.address + partition.size)` over the table = **`0xC50000`**
  (just past `coredump`), identical for the 16 MB and 32 MB layouts (the tables match).
- size = `flash_size - 0xC50000` (flash-size dependent: ~3.6 MB on 16 MB, ~19.7 MB on 32 MB).

The frozen `_boot.py` then `vfs.mount(bdev, "/")`s it; on a blank region the mount fails and
`inisetup.setup()` formats it. `inisetup` formats it **littlefs2** (it keys off the *label*
`"vfs"`, not the FAT subtype the auto-registration assigns). So the internal FS is
**littlefs2, block_size 4096**.

This resolves the "no vfs partition but the FS is real" caveat that
`docs/sd-card-settings-persistence-plan.md` flagged.

## Why bake a launcher: a frozen image has no launcher of its own

A frozen firmware freezes the whole app but nothing calls `Controller.start()` until a flash
`/main.py` runs (see `micropython-frozen-vs-vfs-override.md`). So a freshly flashed image
boots to the REPL until `tools/deploy_app.py`/`tools/set_p4_boot_app.py` writes `/main.py`.
To publish a *self-booting* image, `make dist` bakes `/main.py` into a littlefs2 image and
flashes it into the auto-vfs region.

## Building a MicroPython-compatible littlefs2 image on the host

`tools/mklittlefs_launcher.c` compiles MicroPython's **own vendored** littlefs2
(`deps/micropython/upstream/lib/littlefs/lfs2.c` + `lfs2_util.c`) — so the on-disk format
(disk version 2.1, CRC-32 poly `0x04c11db7`, little-endian) is byte-identical to what the
firmware's `VfsLfs2` mounts. No third-party `littlefs-python`, no disk-version drift, no
network dependency; reproducible in CI (the base image has gcc + these sources).

Only the geometry that lands in the **superblock** must match the firmware to mount:
`block_size = 4096`, `block_count = size / 4096`, and default `name/file/attr_max` (pass 0).
`read/prog/cache/lookahead` are runtime-only; the tool mirrors `VfsLfs2.mkfs`'s defaults
(read/prog 32, `cache_size` 128, `lookahead` 32, `block_cycles` 100) for validity.
`tools/build_launcher_fs.py` derives offset (from the built `partition-table.bin`) and flash
size (from `flash_args`), compiles + runs the tool, and appends `0xc50000 vfs.bin` to the
dist `flash_args`.

## The non-obvious part: force low-block allocation or the image balloons

littlefs seeds its block allocator's start position with `seed % block_count` for
wear-leveling on mount (`lfs2.c:4637`). A freshly formatted FS with one small file therefore
places that file's data block at a pseudo-random position — e.g. block 2039 of 5040 on a
32 MB board — so a truncated image is **~8 MB**, and its size varies with the launcher's
content (the seed derives from the FS).

Fix: after `lfs2_mount`, set `lfs.lookahead.start = 0` before writing the file
(`tools/mklittlefs_launcher.c`). The data block then lands at block 2, and the image
truncates to **3 blocks / 12 KB**. This is purely a wear-leveling hint — the resulting
filesystem is valid littlefs2 and the firmware reads `/main.py` via the on-disk block
pointers regardless of where the block sits. It is safe here because the internal FS is not
written at runtime (user settings live on the SD card), so allocation distribution is moot.

The trailing erased (`0xFF`) blocks are truncated off; esptool writes only the 12 KB prefix,
and the unflashed tail of the partition stays erased on a fresh chip (littlefs tracks free
space via metadata and erases-before-prog, so a stale/unflashed tail is harmless).

## Verified end-to-end (P4-43, 32 MB)

`esptool erase_flash` → `write_flash @flash_args` (firmware + `vfs.bin`) →
`os.listdir('/')` = `['sd', 'main.py']` (**no `boot.py`** ⇒ the baked FS mounted, not an
`inisetup` reformat) → boot log runs the baked `/main.py` → frozen `seedsigner.controller`
imports in ~60 ms → `Controller.start()`. No REPL.

## Scope note

Only the ESP32-P4-43 (release target) publishes this self-booting dist (GitHub CI). The
ESP32-S3 dev boards have no board `manifest.py` / do not freeze the app, so a baked launcher
would `ImportError` → REPL; GitLab/Codeberg (which build S3) intentionally skip the bake
until the S3 frozen-app manifest lands.
