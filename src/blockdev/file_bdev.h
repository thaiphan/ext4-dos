#ifndef FILE_BDEV_H
#define FILE_BDEV_H

#include "blockdev.h"

struct blockdev *file_bdev_open(const char *path);
void             file_bdev_close(struct blockdev *bd);

#endif
