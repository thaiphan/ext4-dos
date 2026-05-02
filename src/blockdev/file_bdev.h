#ifndef FILE_BDEV_H
#define FILE_BDEV_H

#include "blockdev.h"

/* Read-only host file blockdev. Use file_bdev_open_rw for the writable
 * variant — the read-only path is the default so existing tests don't
 * accidentally mutate fixture images. */
struct blockdev *file_bdev_open(const char *path);
struct blockdev *file_bdev_open_rw(const char *path);
void             file_bdev_close(struct blockdev *bd);

#endif
