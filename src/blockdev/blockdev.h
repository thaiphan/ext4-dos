#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include <stdint.h>

#define BDEV_OK         0
#define BDEV_ERR_IO    -1
#define BDEV_ERR_RANGE -2
#define BDEV_ERR_RO    -3   /* device is read-only (write_sectors == NULL) */

struct blockdev;

typedef int (*bdev_read_fn) (struct blockdev *bd, uint64_t lba, uint32_t count, void       *buf);
typedef int (*bdev_write_fn)(struct blockdev *bd, uint64_t lba, uint32_t count, const void *buf);

struct blockdev {
    bdev_read_fn  read_sectors;
    bdev_write_fn write_sectors;   /* NULL = read-only device */
    uint32_t      sector_size;
    uint64_t      total_sectors;
    void         *ctx;
};

static inline int bdev_read(struct blockdev *bd, uint64_t lba, uint32_t count, void *buf) {
    return bd->read_sectors(bd, lba, count, buf);
}

static inline int bdev_write(struct blockdev *bd, uint64_t lba, uint32_t count, const void *buf) {
    if (!bd->write_sectors) return BDEV_ERR_RO;
    return bd->write_sectors(bd, lba, count, buf);
}

static inline int bdev_writable(const struct blockdev *bd) {
    return bd->write_sectors != 0;
}

#endif
