/* Host-side test for phase 2.5 cross-group block allocation.
 *
 * The xgroup fixture was built with blocks_per_group=256 and a filler
 * that completely fills group 0 (free_lo=0). Extending /target.txt
 * therefore forces ext4_file_extend_block to allocate from group 1+.
 *
 * Assertions:
 *   - Extend succeeds
 *   - Allocated block is in group 1 (physical >= first_data_block +
 *     blocks_per_group), proving find_free_block crossed groups
 *   - Block content reads back correctly
 *   - e2fsck-clean post-extend */
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
    const char        *img  = (argc > 1) ? argv[1] : "tests/images/xgroup-test.img";
    struct blockdev   *bd;
    static struct ext4_fs fs;
    static struct ext4_inode inode;
    static uint8_t     new_block[1024];
    static uint8_t     verify[1024];
    char               err[128];
    uint32_t           target_ino;
    uint64_t           orig_size;
    uint64_t           group1_first;   /* first physical block of group 1 */
    uint64_t           alloc_phys;     /* physical block of the new data */
    int                rc, bad;
    size_t             i;
    uint32_t           now;

    bd = file_bdev_open_rw(img);
    ASSERT(bd != NULL, "couldn't open %s rw", img);
    if (!bd) return 1;

    rc = ext4_fs_open(&fs, bd, 0);
    ASSERT(rc == 0, "ext4_fs_open rc=%d", rc);
    ASSERT(fs.jbd.replay_active == 0, "fixture should be clean");

    target_ino = ext4_path_lookup(&fs, "/target.txt");
    ASSERT(target_ino != 0, "/target.txt not found");
    if (target_ino == 0) return 1;

    rc = ext4_inode_read(&fs, target_ino, &inode);
    ASSERT(rc == 0, "read inode rc=%d", rc);
    orig_size = inode.size;
    ASSERT(orig_size == (uint64_t)fs.sb.block_size,
           "/target.txt should be exactly one block, got %lu bytes",
           (unsigned long)orig_size);

    /* group 1 starts at first_data_block + blocks_per_group. Any block
     * allocated there has physical >= group1_first. */
    group1_first = (uint64_t)fs.sb.first_data_block
                 + (uint64_t)fs.sb.blocks_per_group;

    memset(new_block, 'C', sizeof new_block);
    now = inode.mtime + 1u;
    rc = ext4_file_extend_block(&fs, &inode, target_ino, new_block,
                                now, err, sizeof err);
    ASSERT(rc == 0, "ext4_file_extend_block rc=%d err='%s'", rc, err);

    /* Verify size grew. */
    ASSERT(inode.size == orig_size + fs.sb.block_size,
           "inode.size after extend expected %lu, got %lu",
           (unsigned long)(orig_size + fs.sb.block_size),
           (unsigned long)inode.size);

    /* Find the physical block of logical block 1 (the newly extended one). */
    {
        static uint64_t phys;
        rc = ext4_extent_lookup(&fs, inode.i_block, 1u, &phys);
        ASSERT(rc == 0, "extent_lookup for block 1 rc=%d", rc);
        alloc_phys = phys;
    }

    ASSERT(alloc_phys >= group1_first,
           "allocated block %lu is in group 0 (< group1_first=%lu) — "
           "cross-group allocation not triggered",
           (unsigned long)alloc_phys, (unsigned long)group1_first);

    printf("host_xgroup_test: allocated block %lu (group1_first=%lu, "
           "in group %lu)\n",
           (unsigned long)alloc_phys,
           (unsigned long)group1_first,
           (unsigned long)((alloc_phys - fs.sb.first_data_block)
                           / fs.sb.blocks_per_group));

    /* Read back block 1 — should be 'C'. */
    rc = ext4_file_read_block(&fs, &inode, 1u, verify);
    ASSERT(rc == 0, "read block 1 rc=%d", rc);
    bad = 0;
    for (i = 0; i < sizeof verify; i++) if (verify[i] != 'C') bad++;
    ASSERT(bad == 0, "block 1 has %d non-'C' bytes", bad);

    /* Journal must be clean post-commit. */
    ASSERT(fs.jbd.start == 0, "jsb.start non-zero after commit");

    file_bdev_close(bd);

    if (failures == 0) {
        printf("host_xgroup_test: all asserts passed\n");
        return 0;
    }
    fprintf(stderr, "host_xgroup_test: %d FAILURE(S)\n", failures);
    return 1;
}
