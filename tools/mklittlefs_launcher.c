/* mklittlefs_launcher — build a MicroPython-compatible littlefs2 image holding a
 * single file (the frozen-app launcher, written as /main.py) for baking into the
 * ESP32 firmware's internal-flash "vfs" partition.
 *
 * WHY a C tool (not littlefs-python / mtools): it compiles against MicroPython's
 * OWN vendored littlefs2 (deps/micropython/upstream/lib/littlefs/lfs2.c), so the
 * on-disk format is byte-identical to what the firmware's VfsLfs2 formats and
 * mounts — no third-party littlefs, no disk-version drift, no network dependency,
 * reproducible in CI. See tools/build_launcher_fs.py (the wrapper that computes the
 * partition geometry and invokes this) and
 * docs/knowledge/micropython-frozen-vs-vfs-override.md.
 *
 * The config below MIRRORS MicroPython's VfsLfs2.mkfs defaults (extmod/vfs_lfsx.c
 * init_config + the readsize/progsize/lookahead=32 defaults inisetup uses):
 * block_size=4096 (esp32 Partition NATIVE_BLOCK_SIZE_BYTES), block_cycles=100,
 * cache_size=MIN(block_size,4*max(read,prog))=128, lookahead=32, and default
 * name/file/attr_max (0) so the superblock matches. Only the geometry that lands in
 * the superblock (block_size, block_count, *_max) must match for the firmware to
 * mount the image; read/prog/cache/lookahead are runtime-only.
 *
 * Usage: mklittlefs_launcher <out.bin> <block_size> <block_count> <infile> <destname>
 *   e.g. mklittlefs_launcher vfs.bin 4096 5040 main_py.txt main.py
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lfs2.h"

static uint8_t *g_flash;   /* backing "flash": block_size * block_count bytes */

static int bd_read(const struct lfs2_config *c, lfs2_block_t block,
                   lfs2_off_t off, void *buffer, lfs2_size_t size) {
    memcpy(buffer, g_flash + (size_t)block * c->block_size + off, size);
    return 0;
}
static int bd_prog(const struct lfs2_config *c, lfs2_block_t block,
                   lfs2_off_t off, const void *buffer, lfs2_size_t size) {
    memcpy(g_flash + (size_t)block * c->block_size + off, buffer, size);
    return 0;
}
static int bd_erase(const struct lfs2_config *c, lfs2_block_t block) {
    memset(g_flash + (size_t)block * c->block_size, 0xff, c->block_size);
    return 0;
}
static int bd_sync(const struct lfs2_config *c) { (void)c; return 0; }

static void fill_config(struct lfs2_config *cfg, uint32_t block_size, uint32_t block_count) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->read = bd_read;
    cfg->prog = bd_prog;
    cfg->erase = bd_erase;
    cfg->sync = bd_sync;
    cfg->read_size = 32;
    cfg->prog_size = 32;
    cfg->block_size = block_size;
    cfg->block_count = block_count;
    cfg->block_cycles = 100;
    cfg->cache_size = 128;
    cfg->lookahead_size = 32;
    /* name_max/file_max/attr_max/metadata_max/inline_max = 0 -> littlefs defaults,
     * matching MicroPython (which passes 0), so the superblock is identical. */
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s <out.bin> <block_size> <block_count> <infile> <destname>\n", argv[0]);
        return 2;
    }
    const char *out_path = argv[1];
    uint32_t block_size = (uint32_t)strtoul(argv[2], NULL, 0);
    uint32_t block_count = (uint32_t)strtoul(argv[3], NULL, 0);
    const char *in_path = argv[4];
    const char *dest = argv[5];

    if (block_size == 0 || block_count == 0) {
        fprintf(stderr, "error: block_size and block_count must be > 0\n");
        return 2;
    }

    size_t total = (size_t)block_size * block_count;
    g_flash = malloc(total);
    if (!g_flash) { fprintf(stderr, "error: oom allocating %zu bytes\n", total); return 1; }
    memset(g_flash, 0xff, total);   /* erased flash reads as 0xFF */

    /* Read the launcher content into memory. */
    FILE *inf = fopen(in_path, "rb");
    if (!inf) { perror("open infile"); return 1; }
    fseek(inf, 0, SEEK_END);
    long clen = ftell(inf);
    fseek(inf, 0, SEEK_SET);
    if (clen < 0) { fprintf(stderr, "error: cannot size %s\n", in_path); return 1; }
    uint8_t *content = malloc(clen > 0 ? (size_t)clen : 1);
    if (!content) { fprintf(stderr, "error: oom\n"); return 1; }
    if (clen > 0 && fread(content, 1, (size_t)clen, inf) != (size_t)clen) {
        perror("read infile"); return 1;
    }
    fclose(inf);

    struct lfs2_config cfg;
    fill_config(&cfg, block_size, block_count);

    lfs2_t lfs;
    int err = lfs2_format(&lfs, &cfg);
    if (err) { fprintf(stderr, "error: lfs2_format: %d\n", err); return 1; }
    err = lfs2_mount(&lfs, &cfg);
    if (err) { fprintf(stderr, "error: lfs2_mount: %d\n", err); return 1; }

    /* Force the block allocator to scan from block 0 so the file's single data block
     * lands low (block 2), keeping the truncated image tiny. On mount littlefs seeds
     * the allocator start with seed%block_count for wear-leveling (lfs2.c:4637), which
     * would scatter that one data block ~anywhere in the partition and bloat the
     * flashed artifact (e.g. block 2039 -> 8 MB on a 32 MB board). This override is
     * purely a wear-leveling hint: the resulting filesystem is valid littlefs and the
     * firmware reads main.py via the on-disk block pointers regardless of where the
     * data block sits. The internal FS isn't written at runtime (settings live on the
     * SD card), so distribution doesn't matter here. */
    lfs.lookahead.start = 0;

    lfs2_file_t f;
    err = lfs2_file_open(&lfs, &f, dest, LFS2_O_WRONLY | LFS2_O_CREAT | LFS2_O_TRUNC);
    if (err) { fprintf(stderr, "error: lfs2_file_open(%s): %d\n", dest, err); return 1; }
    lfs2_ssize_t wn = lfs2_file_write(&lfs, &f, content, (lfs2_size_t)clen);
    if (wn != (lfs2_ssize_t)clen) {
        fprintf(stderr, "error: lfs2_file_write: %ld != %ld\n", (long)wn, clen); return 1;
    }
    if ((err = lfs2_file_close(&lfs, &f))) { fprintf(stderr, "error: lfs2_file_close: %d\n", err); return 1; }
    if ((err = lfs2_unmount(&lfs))) { fprintf(stderr, "error: lfs2_unmount: %d\n", err); return 1; }

    /* Self-test: remount and read the file back so a host run proves the image is
     * internally consistent (format+write+read round-trips) before we ship it. */
    if ((err = lfs2_mount(&lfs, &cfg))) { fprintf(stderr, "error: verify lfs2_mount: %d\n", err); return 1; }
    if ((err = lfs2_file_open(&lfs, &f, dest, LFS2_O_RDONLY))) {
        fprintf(stderr, "error: verify open(%s): %d\n", dest, err); return 1;
    }
    uint8_t *rb = malloc(clen > 0 ? (size_t)clen : 1);
    lfs2_ssize_t rn = lfs2_file_read(&lfs, &f, rb, (lfs2_size_t)clen);
    if (rn != (lfs2_ssize_t)clen || (clen > 0 && memcmp(rb, content, (size_t)clen) != 0)) {
        fprintf(stderr, "error: verify read-back mismatch (%ld vs %ld)\n", (long)rn, clen); return 1;
    }
    lfs2_file_close(&lfs, &f);
    lfs2_unmount(&lfs);
    free(rb);

    /* Truncate trailing erased (0xFF) blocks so the flashed artifact stays tiny. The
     * unwritten tail of the vfs partition stays erased on a fresh chip; littlefs
     * tracks free space via metadata (not a content scan) and erases-before-prog, so
     * an unflashed/stale tail is harmless. Keep whole blocks up to the last written byte. */
    size_t used = total;
    while (used > 0 && g_flash[used - 1] == 0xff) used--;
    size_t used_blocks = (used + block_size - 1) / block_size;
    if (used_blocks == 0) used_blocks = 1;   /* always keep at least the superblock block */
    used = used_blocks * (size_t)block_size;

    FILE *of = fopen(out_path, "wb");
    if (!of) { perror("open out"); return 1; }
    if (fwrite(g_flash, 1, used, of) != used) { perror("write out"); return 1; }
    fclose(of);

    fprintf(stderr,
            "[mklfs] %s: %zu bytes (%zu of %u blocks x %u), fs geometry %ux%u, "
            "file '%s' = %ld bytes\n",
            out_path, used, used_blocks, block_count, block_size,
            block_size, block_count, dest, clen);
    free(content);
    free(g_flash);
    return 0;
}
