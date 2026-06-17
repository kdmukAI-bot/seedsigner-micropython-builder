# ESP-IDF fatfs vs MicroPython oofatfs: a link-time collision

## Symptom

Linking the MicroPython firmware fails with dozens of duplicate-symbol errors as
soon as the SD card is mounted via ESP-IDF's `board_sdcard_init()`
(`esp_vfs_fat_sdmmc_mount`):

```
ld: esp-idf/fatfs/libfatfs.a(ff.c.obj): in function `f_mount':
  multiple definition of `f_mount';
  esp-idf/main/libmain.a(ff.c.obj): .../lib/oofatfs/ff.c:3389: first defined here
... same for f_open, f_read, f_write, f_close, f_lseek, f_opendir, f_readdir,
    f_stat, f_getfree, f_mkdir, f_unlink, f_rename, ... and get_fattime
```

## Root cause

There are **two complete FATFS implementations** in the tree, and they export the
same `f_*` / `get_fattime` symbols:

1. **MicroPython's `oofatfs`** (`lib/oofatfs/ff.c`), compiled into `libmain.a`.
   This is the FAT layer behind MicroPython's own VFS (`os.mount`,
   `machine.SDCard` + `VfsFat`). It is **always linked** — MicroPython's `os`/VFS
   depends on it.
2. **ESP-IDF's `fatfs` component** (`components/fatfs/src/ff.c`), in `libfatfs.a`,
   used by `esp_vfs_fat_*` (the POSIX `fopen("/sdcard/...")` path).

ESP-IDF's `fatfs` archive is only *pulled into the link* when something references
its symbols. `board_common` lists `fatfs` in `REQUIRES` and compiles
`board_sdcard.c`, but as long as nothing **calls** `board_sdcard_init()`, the
linker never pulls `libfatfs.a(ff.c.obj)` and there's no clash. (This is why
`board_sdcard_init()` was "implemented but never called" — calling it is what
triggers the collision.)

Calling `board_sdcard_init()` → `esp_vfs_fat_sdmmc_mount()` → `f_mount()` forces
`libfatfs.a(ff.c.obj)` into the link, where it collides with oofatfs's `f_mount`
already in `libmain.a`.

You cannot link both. There is no clean `--allow-multiple-definition` escape:
the two `ff.c` builds have different `FFCONF` options and on-disk struct layouts,
so silently binding to the wrong copy would corrupt FAT state.

## Resolution

**Use MicroPython's own FAT VFS for the SD card; never link ESP-IDF's
`esp_vfs_fat`/`fatfs` into the firmware.**

- Mount + read the SD card on the **MicroPython side**: `machine.SDCard(...)`
  (SDMMC; enabled via `MICROPY_HW_ENABLE_SDCARD`, present on the P4) + `vfs.mount`.
- Read the pack file bytes in Python and hand them to the C font loader through
  the `ss_pack_provider_t` seam (`load_locale(locale, packs_dict)` in
  `modseedsigner_bindings.c`). The C side never opens a file.
- Do **not** call `board_sdcard_init()` from the firmware. `board_common` may keep
  `fatfs` in `REQUIRES` (it compiles but isn't linked while unreferenced).

Long filenames (`zh_Hans_CN/zh_Hans_CN.ttf`, `vi/vi_semibold.ttf`) work because
MicroPython's oofatfs has LFN enabled by default (`MICROPY_FATFS_ENABLE_LFN=1`,
`FF_MAX_LFN=255` in `ports/esp32/mpconfigport.h`). No ESP-IDF `CONFIG_FATFS_*`
setting is involved.

## Note for a future SD-everything / secure-boot model

A secure **bootloader** is a *separate binary* from the app, so it may use
ESP-IDF's fatfs to read/verify the SD with no link conflict against the app's
oofatfs. The system then simply has two independent FAT readers. The important
invariant is unchanged: SD contents are untrusted, so anything loaded from SD
(packs today, possibly code later) must be signature-verified by firmware — which
is exactly what the `ss_pack_provider_t` seam is positioned to do.
