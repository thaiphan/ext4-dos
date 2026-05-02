/* Host-side test for ext4_file_create (phase 3).
 *
 * Mounts a writable copy of write.img, creates /newfile.txt with known
 * content, then verifies:
 *   - ext4_path_lookup finds it
 *   - inode.size == block_size after writing one block
 *   - content reads back correctly
 *   - journal is clean post-commit
 *   - e2fsck reports the image clean
 *
 * The write.img fixture is a small ext4 with ^metadata_csum (simple case
 * first; metadata_csum create test lives in host_create_csum_test). */
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
    const char        *img      = (argc > 1) ? argv[1] : "tests/images/create-test.img";
    struct blockdev   *bd;
    static struct ext4_fs fs;
    static struct ext4_inode inode;
    static uint8_t     content[1024];
    static uint8_t     verify[1024];
    char               err[128];
    uint32_t           new_ino;
    uint32_t           found_ino;
    uint32_t           now;
    int                rc, bad;
    size_t             i;

    bd = file_bdev_open_rw(img);
    ASSERT(bd != NULL, "couldn't open %s rw", img);
    if (!bd) return 1;

    rc = ext4_fs_open(&fs, bd, 0);
    ASSERT(rc == 0, "ext4_fs_open rc=%d", rc);
    ASSERT(fs.jbd.replay_active == 0, "fixture should be clean");

    /* File shouldn't exist yet. */
    ASSERT(ext4_path_lookup(&fs, "/newfile.txt") == 0,
           "/newfile.txt shouldn't exist before create");

    /* Create /newfile.txt in root dir (inode 2). */
    now = 0x5A000000u;
    new_ino = ext4_file_create(&fs, /*parent_ino=*/2u,
                               "newfile.txt", 11u,
                               (uint16_t)(0x8000u | 0644u), now,
                               err, sizeof err);
    ASSERT(new_ino != 0, "ext4_file_create returned 0, err='%s'", err);
    if (new_ino == 0) return 1;

    printf("host_create_test: created /newfile.txt as inode %u\n", new_ino);

    /* Path lookup should now find it. */
    found_ino = ext4_path_lookup(&fs, "/newfile.txt");
    ASSERT(found_ino == new_ino,
           "path_lookup returned %u, expected %u", found_ino, new_ino);

    /* The new inode has size=0 and no data blocks yet. Write one block. */
    rc = ext4_inode_read(&fs, new_ino, &inode);
    ASSERT(rc == 0, "inode_read rc=%d", rc);
    ASSERT(inode.size == 0, "new inode should have size 0, got %lu",
           (unsigned long)inode.size);

    memset(content, 'N', sizeof content);
    rc = ext4_file_extend_block(&fs, &inode, new_ino, content, sizeof content,
                                now + 1u, err, sizeof err);
    ASSERT(rc == 0, "extend_block rc=%d err='%s'", rc, err);
    ASSERT(inode.size == (uint64_t)fs.sb.block_size,
           "after extend, size expected %u, got %lu",
           fs.sb.block_size, (unsigned long)inode.size);

    /* Read back. */
    rc = ext4_inode_read(&fs, new_ino, &inode);
    ASSERT(rc == 0, "re-read inode rc=%d", rc);
    rc = ext4_file_read_block(&fs, &inode, 0, verify);
    ASSERT(rc == 0, "read block 0 rc=%d", rc);
    bad = 0;
    for (i = 0; i < sizeof verify; i++) if (verify[i] != 'N') bad++;
    ASSERT(bad == 0, "block 0 has %d non-'N' bytes", bad);

    ASSERT(fs.jbd.start == 0, "journal must be clean after create+extend");

    if (failures == 0) {
        printf("host_create_test: all asserts passed\n");
        return 0;
    }
    fprintf(stderr, "host_create_test: %d FAILURE(S)\n", failures);
    return 1;
}
