#include "file_bdev.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct file_bdev_ctx {
    FILE *f;
};

static int file_bdev_read(struct blockdev *bd, uint64_t lba, uint32_t count, void *buf) {
    struct file_bdev_ctx *c = (struct file_bdev_ctx *)bd->ctx;
    off_t off = (off_t)(lba * bd->sector_size);
    if (fseeko(c->f, off, SEEK_SET) != 0) return BDEV_ERR_IO;
    size_t want = (size_t)count * bd->sector_size;
    if (fread(buf, 1, want, c->f) != want) return BDEV_ERR_IO;
    return BDEV_OK;
}

struct blockdev *file_bdev_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    struct stat st;
    if (fstat(fileno(f), &st) != 0) { fclose(f); return NULL; }

    struct file_bdev_ctx *c = (struct file_bdev_ctx *)calloc(1, sizeof *c);
    if (!c) { fclose(f); return NULL; }
    c->f = f;

    struct blockdev *bd = (struct blockdev *)calloc(1, sizeof *bd);
    if (!bd) { free(c); fclose(f); return NULL; }
    bd->read_sectors  = file_bdev_read;
    bd->sector_size   = 512;
    bd->total_sectors = (uint64_t)st.st_size / 512u;
    bd->ctx           = c;
    return bd;
}

void file_bdev_close(struct blockdev *bd) {
    if (!bd) return;
    struct file_bdev_ctx *c = (struct file_bdev_ctx *)bd->ctx;
    if (c) {
        if (c->f) fclose(c->f);
        free(c);
    }
    free(bd);
}
