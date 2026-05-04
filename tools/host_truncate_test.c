/* Host-side test for ext4_file_truncate.
 *
 * Three cases:
 *   1. Truncate-to-zero (3 blocks -> 0): all extents removed, size=0
 *   2. Truncate to block boundary (3 blocks -> 1 block): one extent kept
 *   3. Truncate to mid-block (3 blocks -> 1500 bytes): one extent kept,
 *      size set to non-block-aligned value
 *
 * After each: post-truncate read must see the new size, journal clean,
 * underlying image must remain e2fsck-clean (not run here; the freedos-
 * test / msdos4-test wrappers run e2fsck at the end of a boot). */

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

static uint32_t make_three_block_file(struct ext4_fs *fs, const char *name,
                                      uint8_t name_len, uint32_t now) {
    static uint8_t content[1024];
    static struct ext4_inode inode;
    char err[128];
    uint32_t ino;
    int rc;

    ino = ext4_file_create(fs, 2u, name, name_len,
                           (uint16_t)(0x8000u | 0644u), now, err, sizeof err);
    ASSERT(ino != 0, "create %s: err='%s'", name, err);
    if (!ino) return 0;

    memset(content, 'A', sizeof content);
    rc = ext4_inode_read(fs, ino, &inode);
    ASSERT(rc == 0, "inode read rc=%d", rc);

    rc = ext4_file_extend_block(fs, &inode, ino, content, sizeof content,
                                now+1u, err, sizeof err);
    ASSERT(rc == 0, "extend 1 rc=%d err='%s'", rc, err);
    memset(content, 'B', sizeof content);
    rc = ext4_file_extend_block(fs, &inode, ino, content, sizeof content,
                                now+2u, err, sizeof err);
    ASSERT(rc == 0, "extend 2 rc=%d err='%s'", rc, err);
    memset(content, 'C', sizeof content);
    rc = ext4_file_extend_block(fs, &inode, ino, content, sizeof content,
                                now+3u, err, sizeof err);
    ASSERT(rc == 0, "extend 3 rc=%d err='%s'", rc, err);

    return ino;
}

int main(int argc, char **argv) {
    const char       *img = (argc > 1) ? argv[1] : "tests/images/truncate-test.img";
    struct blockdev  *bd;
    static struct ext4_fs fs;
    static struct ext4_inode inode;
    char              err[128];
    uint32_t          ino;
    uint32_t          now = 0x5B000000u;
    uint64_t          init_free;
    int               rc;

    bd = file_bdev_open_rw(img);
    ASSERT(bd != NULL, "couldn't open %s rw", img);
    if (!bd) return 1;

    rc = ext4_fs_open(&fs, bd, 0);
    ASSERT(rc == 0, "ext4_fs_open rc=%d", rc);
    init_free = fs.sb.free_blocks_count;

    /* --- Case 1: truncate-to-zero --- */
    ino = make_three_block_file(&fs, "tz.txt", 6u, now);
    if (!ino) goto done;
    rc = ext4_file_truncate(&fs, ino, 0u, now+10u, err, sizeof err);
    ASSERT(rc == 0, "truncate-to-zero rc=%d err='%s'", rc, err);
    rc = ext4_inode_read(&fs, ino, &inode);
    ASSERT(rc == 0, "post-truncate inode_read rc=%d", rc);
    ASSERT(inode.size == 0, "size after trunc-to-zero: got %llu (want 0)",
           (unsigned long long)inode.size);
    {
        const uint8_t *iblk = inode.i_block;
        uint16_t ents = (uint16_t)(iblk[2] | (iblk[3] << 8));
        ASSERT(ents == 0, "extent entries after trunc-to-zero: got %u (want 0)", ents);
    }
    /* All 3 blocks freed → free count back to start. */
    ASSERT(fs.sb.free_blocks_count == init_free + 0u,
           "free_blocks after trunc-to-zero: got %llu, init was %llu",
           (unsigned long long)fs.sb.free_blocks_count,
           (unsigned long long)init_free);
    printf("host_truncate_test: truncate-to-zero on 3-block file OK\n");

    /* Re-create for next case (different name to avoid path collision). */
    ino = make_three_block_file(&fs, "tb.txt", 6u, now+20u);
    if (!ino) goto done;
    /* --- Case 2: truncate to 1-block boundary (1024 bytes) --- */
    rc = ext4_file_truncate(&fs, ino, 1024u, now+30u, err, sizeof err);
    ASSERT(rc == 0, "truncate-to-1024 rc=%d err='%s'", rc, err);
    rc = ext4_inode_read(&fs, ino, &inode);
    ASSERT(rc == 0, "post-truncate inode_read rc=%d", rc);
    ASSERT(inode.size == 1024u, "size after trunc-to-1024: got %llu",
           (unsigned long long)inode.size);
    {
        const uint8_t *iblk = inode.i_block;
        uint16_t ents = (uint16_t)(iblk[2] | (iblk[3] << 8));
        ASSERT(ents == 1, "extent entries after trunc-to-1024: got %u (want 1)", ents);
    }
    printf("host_truncate_test: truncate to 1-block boundary OK\n");

    /* --- Case 3: truncate to mid-block (1500 bytes) --- */
    ino = make_three_block_file(&fs, "tm.txt", 6u, now+40u);
    if (!ino) goto done;
    rc = ext4_file_truncate(&fs, ino, 1500u, now+50u, err, sizeof err);
    ASSERT(rc == 0, "truncate-to-1500 rc=%d err='%s'", rc, err);
    rc = ext4_inode_read(&fs, ino, &inode);
    ASSERT(rc == 0, "post-truncate inode_read rc=%d", rc);
    ASSERT(inode.size == 1500u, "size after trunc-to-1500: got %llu",
           (unsigned long long)inode.size);
    {
        /* 1500 bytes spans 2 blocks (ceil), so 2 blocks kept. */
        uint8_t buf[1024];
        rc = ext4_file_read_block(&fs, &inode, 0, buf);
        ASSERT(rc == 0, "read_block 0 rc=%d", rc);
        ASSERT(buf[0] == 'A', "block 0 should still be 'A'");
        rc = ext4_file_read_block(&fs, &inode, 1, buf);
        ASSERT(rc == 0, "read_block 1 rc=%d", rc);
        ASSERT(buf[0] == 'B', "block 1 should still be 'B'");
    }
    printf("host_truncate_test: truncate to mid-block (1500 bytes) OK\n");

    /* --- Case 4: same-size no-op --- */
    rc = ext4_file_truncate(&fs, ino, 1500u, now+60u, err, sizeof err);
    ASSERT(rc == 0, "no-op truncate rc=%d err='%s'", rc, err);
    printf("host_truncate_test: same-size no-op OK\n");

    /* --- Case 5: refuse extend (new_size > current) --- */
    rc = ext4_file_truncate(&fs, ino, 4096u, now+70u, err, sizeof err);
    ASSERT(rc != 0, "extend-via-truncate should fail");
    printf("host_truncate_test: extend-via-truncate refused OK\n");

    ASSERT(fs.jbd.start == 0, "journal must be clean");

done:
    if (failures == 0) {
        printf("host_truncate_test: all asserts passed\n");
        return 0;
    }
    fprintf(stderr, "host_truncate_test: %d FAILURE(S)\n", failures);
    return 1;
}
