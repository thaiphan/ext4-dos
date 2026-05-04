#include "file_bdev.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct file_bdev_ctx {
    FILE *f;
    int   fail_after; /* >=0: writes left to allow before failing; <0: never */
};

static int file_bdev_read(struct blockdev *bd, uint64_t lba, uint32_t count, void *buf) {
    struct file_bdev_ctx *c = (struct file_bdev_ctx *)bd->ctx;
    off_t off = (off_t)(lba * bd->sector_size);
    if (fseeko(c->f, off, SEEK_SET) != 0) return BDEV_ERR_IO;
    size_t want = (size_t)count * bd->sector_size;
    if (fread(buf, 1, want, c->f) != want) return BDEV_ERR_IO;
    return BDEV_OK;
}

static int file_bdev_write(struct blockdev *bd, uint64_t lba, uint32_t count, const void *buf) {
    struct file_bdev_ctx *c = (struct file_bdev_ctx *)bd->ctx;
    off_t off = (off_t)(lba * bd->sector_size);
    if (c->fail_after >= 0) {
        if (c->fail_after == 0) return BDEV_ERR_IO;
        c->fail_after--;
    }
    if (fseeko(c->f, off, SEEK_SET) != 0) return BDEV_ERR_IO;
    size_t want = (size_t)count * bd->sector_size;
    if (fwrite(buf, 1, want, c->f) != want) return BDEV_ERR_IO;
    if (fflush(c->f) != 0) return BDEV_ERR_IO;
    return BDEV_OK;
}

static struct blockdev *open_with_mode(const char *path, const char *mode, int writable) {
    FILE *f = fopen(path, mode);
    if (!f) return NULL;

    struct stat st;
    if (fstat(fileno(f), &st) != 0) { fclose(f); return NULL; }

    struct file_bdev_ctx *c = (struct file_bdev_ctx *)calloc(1, sizeof *c);
    if (!c) { fclose(f); return NULL; }
    c->f          = f;
    c->fail_after = -1;

    struct blockdev *bd = (struct blockdev *)calloc(1, sizeof *bd);
    if (!bd) { free(c); fclose(f); return NULL; }
    bd->read_sectors  = file_bdev_read;
    bd->write_sectors = writable ? file_bdev_write : NULL;
    bd->sector_size   = 512;
    bd->total_sectors = (uint64_t)st.st_size / 512u;
    bd->ctx           = c;
    return bd;
}

struct blockdev *file_bdev_open(const char *path) {
    return open_with_mode(path, "rb", 0);
}

struct blockdev *file_bdev_open_rw(const char *path) {
    return open_with_mode(path, "rb+", 1);
}

void file_bdev_set_fail_after(struct blockdev *bd, int n) {
    if (!bd || !bd->ctx) return;
    ((struct file_bdev_ctx *)bd->ctx)->fail_after = n;
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
