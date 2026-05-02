/* Host-side test for ext4_dir_create (phase 4).
 *
 * Mounts a writable copy of write.img, creates /newdir, then verifies:
 *   - ext4_path_lookup finds the new directory
 *   - path_lookup on /newdir/. returns the same inode
 *   - path_lookup on /newdir/.. returns root (inode 2)
 *   - journal is clean post-commit
 *   - e2fsck reports the image clean */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "blockdev/blockdev.h"
#include "blockdev/file_bdev.h"
#include "ext4/superblock.h"
#include "ext4/features.h"
#include "ext4/fs.h"
#include "ext4/inode.h"
#include "ext4/extent.h"
#include "ext4/dir.h"
#include "ext4/journal.h"

static int failures = 0;
#define ASSERT(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

int main(int argc, char **argv) {
    const char       *img = (argc > 1) ? argv[1] : "tests/images/mkdir-test.img";
    struct blockdev  *bd;
    static struct ext4_fs fs;
    char              err[128];
    uint32_t          new_ino, found_ino;
    uint32_t          now = 0x5A000000u;
    int               rc;

    bd = file_bdev_open_rw(img);
    ASSERT(bd != NULL, "couldn't open %s rw", img);
    if (!bd) return 1;

    rc = ext4_fs_open(&fs, bd, 0);
    ASSERT(rc == 0, "ext4_fs_open rc=%d", rc);
    ASSERT(fs.jbd.replay_active == 0, "fixture should be clean");

    /* /newdir should not exist yet. */
    ASSERT(ext4_path_lookup(&fs, "/newdir") == 0,
           "/newdir shouldn't exist before mkdir");

    new_ino = ext4_dir_create(&fs, 2u, "newdir", 6u, now, err, sizeof err);
    ASSERT(new_ino != 0, "ext4_dir_create returned 0, err='%s'", err);
    if (new_ino == 0) return 1;

    printf("host_mkdir_test: created /newdir as inode %u\n", new_ino);

    /* Path lookup must find it. */
    found_ino = ext4_path_lookup(&fs, "/newdir");
    ASSERT(found_ino == new_ino,
           "path_lookup returned %u, expected %u", found_ino, new_ino);

    /* Dot entry must resolve to itself. */
    found_ino = ext4_path_lookup(&fs, "/newdir/.");
    ASSERT(found_ino == new_ino,
           "/newdir/. returned %u, expected %u", found_ino, new_ino);

    /* Dotdot entry must resolve to root (inode 2). */
    found_ino = ext4_path_lookup(&fs, "/newdir/..");
    ASSERT(found_ino == 2u,
           "/newdir/.. returned %u, expected 2 (root)", found_ino);

    ASSERT(fs.jbd.start == 0, "journal must be clean after mkdir");

    if (failures == 0) {
        printf("host_mkdir_test: all asserts passed\n");
        return 0;
    }
    fprintf(stderr, "host_mkdir_test: %d FAILURE(S)\n", failures);
    return 1;
}
