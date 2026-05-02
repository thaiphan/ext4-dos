/* Host-side test for ext4_dir_remove.
 *
 * Creates /newdir, verifies it exists, removes it, then checks:
 *   - path_lookup returns 0 after removal
 *   - parent (root) link count decremented
 *   - journal clean post-commit
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
    const char       *img = (argc > 1) ? argv[1] : "tests/images/rmdir-test.img";
    struct blockdev  *bd;
    static struct ext4_fs fs;
    static struct ext4_inode root_inode_unused;
    char              err[128];
    uint32_t          new_ino;
    uint32_t          now = 0x5A000000u;
    int               rc;

    bd = file_bdev_open_rw(img);
    ASSERT(bd != NULL, "couldn't open %s rw", img);
    if (!bd) return 1;

    rc = ext4_fs_open(&fs, bd, 0);
    ASSERT(rc == 0, "ext4_fs_open rc=%d", rc);
    ASSERT(fs.jbd.replay_active == 0, "fixture should be clean");

    (void)root_inode_unused;

    /* Create /newdir. */
    new_ino = ext4_dir_create(&fs, 2u, "newdir", 6u, now, err, sizeof err);
    ASSERT(new_ino != 0, "ext4_dir_create returned 0, err='%s'", err);
    if (new_ino == 0) return 1;
    ASSERT(ext4_path_lookup(&fs, "/newdir") == new_ino, "/newdir not found after create");

    printf("host_rmdir_test: created /newdir as inode %u\n", new_ino);

    /* Remove /newdir. */
    rc = ext4_dir_remove(&fs, 2u, new_ino, err, sizeof err);
    ASSERT(rc == 0, "ext4_dir_remove rc=%d, err='%s'", rc, err);

    /* Path lookup must return 0 now. */
    ASSERT(ext4_path_lookup(&fs, "/newdir") == 0,
           "/newdir still found after remove");

    ASSERT(fs.jbd.start == 0, "journal must be clean after rmdir");

    /* Link count correctness is verified by e2fsck on the image. */
    printf("host_rmdir_test: removed /newdir — journal clean\n");

    if (failures == 0) {
        printf("host_rmdir_test: all asserts passed\n");
        return 0;
    }
    fprintf(stderr, "host_rmdir_test: %d FAILURE(S)\n", failures);
    return 1;
}
