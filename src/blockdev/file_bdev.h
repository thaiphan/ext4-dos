#ifndef FILE_BDEV_H
#define FILE_BDEV_H

#include "blockdev.h"

/* Read-only host file blockdev. Use file_bdev_open_rw for the writable
 * variant — the read-only path is the default so existing tests don't
 * accidentally mutate fixture images. */
struct blockdev *file_bdev_open(const char *path);
struct blockdev *file_bdev_open_rw(const char *path);
void             file_bdev_close(struct blockdev *bd);

/* Fault injection (test-only). Allow `n` more writes to succeed; the
 * (n+1)-th and subsequent writes return BDEV_ERR_IO. n=0 fails the
 * very next write. n<0 disables fault injection (the default).
 * Used by host_crash_recovery_test to probe replay/checkpoint
 * idempotency at every internal write point. */
void             file_bdev_set_fail_after(struct blockdev *bd, int n);

#endif
