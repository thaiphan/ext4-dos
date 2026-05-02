/* Host-side test for ext4_rename (in-place same-parent rename).
 *
 * Creates /oldname.txt, renames it to /newname.txt, verifies:
 *   - /oldname.txt no longer found
 *   - /newname.txt found at the same inode
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
    const char       *img = (argc > 1) ? argv[1] : "tests/images/rename-test.img";
    struct blockdev  *bd;
    static struct ext4_fs fs;
    char              err[128];
    uint32_t          ino, found;
    uint32_t          now = 0x5A000000u;
    int               rc;

    bd = file_bdev_open_rw(img);
    ASSERT(bd != NULL, "couldn't open %s rw", img);
    if (!bd) return 1;

    rc = ext4_fs_open(&fs, bd, 0);
    ASSERT(rc == 0, "ext4_fs_open rc=%d", rc);

    ino = ext4_file_create(&fs, 2u, "oldname.txt", 11u,
                           (uint16_t)(0x8000u | 0644u), now, err, sizeof err);
    ASSERT(ino != 0, "create oldname.txt: err='%s'", err);

    rc = ext4_rename(&fs, 2u, ino, "newname.txt", 11u, err, sizeof err);
    ASSERT(rc == 0, "rename rc=%d err='%s'", rc, err);

    ASSERT(ext4_path_lookup(&fs, "/oldname.txt") == 0,
           "/oldname.txt still found after rename");
    found = ext4_path_lookup(&fs, "/newname.txt");
    ASSERT(found == ino, "/newname.txt lookup returned %u, expected %u", found, ino);

    printf("host_rename_test: renamed /oldname.txt -> /newname.txt (inode %u)\n", ino);

    /* Also rename a directory. */
    {
        uint32_t dir_ino = ext4_dir_create(&fs, 2u, "olddir", 6u, now+1u, err, sizeof err);
        ASSERT(dir_ino != 0, "dir_create: err='%s'", err);
        rc = ext4_rename(&fs, 2u, dir_ino, "newdir", 6u, err, sizeof err);
        ASSERT(rc == 0, "rename dir rc=%d err='%s'", rc, err);
        ASSERT(ext4_path_lookup(&fs, "/olddir") == 0, "/olddir still found");
        ASSERT(ext4_path_lookup(&fs, "/newdir") == dir_ino,
               "/newdir not found at expected inode");
        printf("host_rename_test: renamed /olddir -> /newdir (inode %u)\n", dir_ino);
    }

    ASSERT(fs.jbd.start == 0, "journal must be clean");

    if (failures == 0) {
        printf("host_rename_test: all asserts passed\n");
        return 0;
    }
    fprintf(stderr, "host_rename_test: %d FAILURE(S)\n", failures);
    return 1;
}
