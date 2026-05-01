#ifndef INT13_BDEV_H
#define INT13_BDEV_H

#include "blockdev.h"

struct blockdev *int13_bdev_open(uint8_t drive);
void             int13_bdev_close(struct blockdev *bd);

#endif
