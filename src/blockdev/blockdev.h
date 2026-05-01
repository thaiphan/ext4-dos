#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include <stdint.h>

#define BDEV_OK         0
#define BDEV_ERR_IO    -1
#define BDEV_ERR_RANGE -2

struct blockdev;

typedef int (*bdev_read_fn)(struct blockdev *bd, uint64_t lba, uint32_t count, void *buf);

struct blockdev {
    bdev_read_fn read_sectors;
    uint32_t     sector_size;
    uint64_t     total_sectors;
    void        *ctx;
};

static inline int bdev_read(struct blockdev *bd, uint64_t lba, uint32_t count, void *buf) {
    return bd->read_sectors(bd, lba, count, buf);
}

#endif
