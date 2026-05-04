/* Host-side test for ext4_rename_xdir (cross-directory rename).
 *
 * Setup: /xfoo.txt (1 block of 'X') exists in root; /xsub/ exists (created here).
 * Test:
 *   1. Move /xfoo.txt -> /xsub/xfoo.txt  (same name, new parent)
 *   2. Move /xsub/xfoo.txt -> /xrenamed.txt  (new name, root again)
 *   3. Verify the inode number is preserved across both moves
 *   4. Same-parent must be refused (we exposed ext4_rename for that)
 *   5. Refuse cross-dir rename of a directory (out of scope)
 *
 * Note: not compiled into DOS builds — host-only verification. */
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
    const char       *img = (argc > 1) ? argv[1] : "tests/images/rename-xdir-test.img";
    struct blockdev  *bd;
    static struct ext4_fs fs;
    static struct ext4_inode inode;
    static uint8_t   content[1024];
    char              err[128];
    uint32_t          file_ino, sub_ino;
    uint32_t          now = 0x5C000000u;
    int               rc;

    bd = file_bdev_open_rw(img);
    ASSERT(bd != NULL, "couldn't open %s rw", img);
    if (!bd) return 1;

    rc = ext4_fs_open(&fs, bd, 0);
    ASSERT(rc == 0, "ext4_fs_open rc=%d", rc);

    /* Setup: create /xfoo.txt (1 block) and /xsub/ */
    file_ino = ext4_file_create(&fs, 2u, "xfoo.txt", 8u,
                                (uint16_t)(0x8000u | 0644u), now, err, sizeof err);
    ASSERT(file_ino != 0, "create xfoo.txt: '%s'", err);
    if (file_ino) {
        rc = ext4_inode_read(&fs, file_ino, &inode);
        ASSERT(rc == 0, "inode read");
        memset(content, 'X', sizeof content);
        rc = ext4_file_extend_block(&fs, &inode, file_ino, content, sizeof content,
                                    now+1u, err, sizeof err);
        ASSERT(rc == 0, "extend xfoo.txt: '%s'", err);
    }
    sub_ino = ext4_dir_create(&fs, 2u, "xsub", 4u, now+2u, err, sizeof err);
    ASSERT(sub_ino != 0, "mkdir xsub: '%s'", err);

    if (failures || !file_ino || !sub_ino) goto done;

    /* --- 1. /xfoo.txt -> /xsub/xfoo.txt --- */
    rc = ext4_rename_xdir(&fs, 2u, file_ino, sub_ino,
                          "xfoo.txt", 8u, now+10u, err, sizeof err);
    ASSERT(rc == 0, "xdir rename 1: '%s'", err);
    ASSERT(ext4_path_lookup(&fs, "/xfoo.txt") == 0, "xfoo.txt still in root");
    ASSERT(ext4_path_lookup(&fs, "/xsub/xfoo.txt") == file_ino,
           "xsub/xfoo.txt missing or wrong inode");

    /* --- 2. /xsub/xfoo.txt -> /xrenamed.txt (new parent + new name) --- */
    rc = ext4_rename_xdir(&fs, sub_ino, file_ino, 2u,
                          "xrenamed.txt", 12u, now+20u, err, sizeof err);
    ASSERT(rc == 0, "xdir rename 2: '%s'", err);
    ASSERT(ext4_path_lookup(&fs, "/xsub/xfoo.txt") == 0,
           "xsub/xfoo.txt still found after move-out");
    ASSERT(ext4_path_lookup(&fs, "/xrenamed.txt") == file_ino,
           "xrenamed.txt missing or wrong inode");

    /* Content must still read 'X' across the renames. */
    rc = ext4_inode_read(&fs, file_ino, &inode);
    ASSERT(rc == 0, "post-rename inode read");
    rc = ext4_file_read_block(&fs, &inode, 0, content);
    ASSERT(rc == 0, "post-rename data read");
    ASSERT(content[0] == 'X' && content[1023] == 'X', "data corrupted");

    /* --- 3. Same-parent must be refused --- */
    rc = ext4_rename_xdir(&fs, 2u, file_ino, 2u,
                          "samepar.txt", 11u, now+30u, err, sizeof err);
    ASSERT(rc != 0, "same-parent xdir rename should fail");

    /* --- 4. Empty name must be refused --- */
    rc = ext4_rename_xdir(&fs, 2u, file_ino, sub_ino,
                          "", 0u, now+40u, err, sizeof err);
    ASSERT(rc != 0, "empty-name xdir rename should fail");

    /* --- 5. Renaming a directory must be refused --- */
    rc = ext4_rename_xdir(&fs, 2u, sub_ino, sub_ino,
                          "xsubmoved", 9u, now+50u, err, sizeof err);
    ASSERT(rc != 0, "directory xdir rename should fail (.. update not supported)");

    ASSERT(fs.jbd.start == 0, "journal must be clean");

    if (failures == 0) {
        printf("host_rename_xdir_test: all asserts passed\n");
        return 0;
    }
done:
    fprintf(stderr, "host_rename_xdir_test: %d FAILURE(S)\n", failures);
    return 1;
}
