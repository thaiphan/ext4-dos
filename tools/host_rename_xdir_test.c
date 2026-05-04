/* Host-side test for ext4_rename_xdir (cross-directory rename).
 *
 * Setup: /xfoo.txt (1 block of 'X'), /xsub/ exist in root. The dir-rename
 * cases also create /xdir1/, /xdir1/leaf.txt, and /xdir2/.
 *
 * Test:
 *   File rename:
 *     1. Move /xfoo.txt -> /xsub/xfoo.txt  (same name, new parent)
 *     2. Move /xsub/xfoo.txt -> /xrenamed.txt  (new name, root again)
 *     3. Verify the inode number is preserved across both moves
 *     4. Same-parent must be refused
 *     5. Empty name must be refused
 *   Directory rename:
 *     6. Move /xdir1 -> /xsub/xdir1 (with leaf.txt inside)
 *     7. Verify "..'s inode in xdir1's first block now points at xsub
 *     8. Verify nlinks: root -1, xsub +1 (relative to pre-move snapshot)
 *     9. Move /xsub/xdir1 -> /xdir2/sub (with new name)
 *    10. Refuse cycle: move /xdir2 -> /xdir2/sub (xdir2 is now an ancestor)
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

    /* --- 5. Renaming root must be refused --- */
    rc = ext4_rename_xdir(&fs, 2u, 2u, sub_ino,
                          "rootmoved", 9u, now+50u, err, sizeof err);
    ASSERT(rc != 0, "rename of root must be refused");

    /* --- 6. Cross-dir DIRECTORY rename: /xdir1 -> /xsub/xdir1 --- */
    {
        uint32_t xdir1_ino, xdir2_ino, leaf_ino;
        uint32_t pre_root_nlinks, pre_xsub_nlinks, post_root_nlinks, post_xsub_nlinks;
        static struct ext4_inode dir_inode;
        static uint8_t dir_block[1024];
        uint32_t dotdot_ino;

        xdir1_ino = ext4_dir_create(&fs, 2u, "xdir1", 5u, now+60u, err, sizeof err);
        ASSERT(xdir1_ino != 0, "mkdir /xdir1: '%s'", err);
        xdir2_ino = ext4_dir_create(&fs, 2u, "xdir2", 5u, now+61u, err, sizeof err);
        ASSERT(xdir2_ino != 0, "mkdir /xdir2: '%s'", err);
        leaf_ino  = ext4_file_create(&fs, xdir1_ino, "leaf.txt", 8u,
                                     (uint16_t)(0x8000u | 0644u), now+62u,
                                     err, sizeof err);
        ASSERT(leaf_ino != 0, "create /xdir1/leaf.txt: '%s'", err);

        /* Snapshot link counts of root and xsub before the move. */
        {
            static uint8_t inblk[1024];
            uint32_t off;
            uint64_t fb;
            uint32_t g, idx;
            const uint8_t *bgd;
            uint64_t it;

            for (int phase = 0; phase < 2; phase++) {
                uint32_t ino = (phase == 0) ? 2u : sub_ino;
                g   = (ino - 1u) / fs.sb.inodes_per_group;
                idx = (ino - 1u) % fs.sb.inodes_per_group;
                bgd = fs.bgd_buf + g * fs.bgd_size;
                it  = ((uint64_t)((fs.bgd_size>=64u)?(((uint32_t)bgd[0x28])
                       |((uint32_t)bgd[0x29]<<8)|((uint32_t)bgd[0x2A]<<16)
                       |((uint32_t)bgd[0x2B]<<24)) : 0u) << 32)
                    | (((uint32_t)bgd[0x08])|((uint32_t)bgd[0x09]<<8)
                       |((uint32_t)bgd[0x0A]<<16)|((uint32_t)bgd[0x0B]<<24));
                fb  = it + (uint64_t)idx * fs.sb.inode_size / fs.sb.block_size;
                off = (uint32_t)((uint64_t)idx * fs.sb.inode_size % fs.sb.block_size);
                rc  = ext4_fs_read_block(&fs, fb, inblk);
                ASSERT(rc == 0, "snapshot inode read");
                {
                    uint32_t nl = (uint32_t)inblk[off+0x1A] | ((uint32_t)inblk[off+0x1B]<<8);
                    if (phase == 0) pre_root_nlinks = nl;
                    else            pre_xsub_nlinks = nl;
                }
            }
        }

        rc = ext4_rename_xdir(&fs, 2u, xdir1_ino, sub_ino,
                              "xdir1", 5u, now+70u, err, sizeof err);
        ASSERT(rc == 0, "xdir DIR rename: '%s'", err);
        ASSERT(ext4_path_lookup(&fs, "/xdir1") == 0, "/xdir1 still in root");
        ASSERT(ext4_path_lookup(&fs, "/xsub/xdir1") == xdir1_ino,
               "/xsub/xdir1 missing or wrong inode");
        ASSERT(ext4_path_lookup(&fs, "/xsub/xdir1/leaf.txt") == leaf_ino,
               "/xsub/xdir1/leaf.txt missing — child entry not preserved");

        /* --- 7. Verify ..'s inode points at xsub --- */
        rc = ext4_inode_read(&fs, xdir1_ino, &dir_inode);
        ASSERT(rc == 0, "moved dir inode read");
        {
            uint64_t phys;
            rc = ext4_extent_lookup(&fs, dir_inode.i_block, 0, &phys);
            ASSERT(rc == 0, "moved dir extent lookup");
            rc = ext4_fs_read_block(&fs, phys, dir_block);
            ASSERT(rc == 0, "moved dir block read");
        }
        dotdot_ino = (uint32_t)dir_block[12]
                   | ((uint32_t)dir_block[13] << 8)
                   | ((uint32_t)dir_block[14] << 16)
                   | ((uint32_t)dir_block[15] << 24);
        ASSERT(dotdot_ino == sub_ino, "..'s inode is %u, expected sub_ino=%u",
               (unsigned)dotdot_ino, (unsigned)sub_ino);

        /* --- 8. Verify nlinks: root -1, xsub +1 --- */
        {
            static uint8_t inblk[1024];
            uint32_t off;
            uint64_t fb;
            uint32_t g, idx;
            const uint8_t *bgd;
            uint64_t it;

            for (int phase = 0; phase < 2; phase++) {
                uint32_t ino = (phase == 0) ? 2u : sub_ino;
                g   = (ino - 1u) / fs.sb.inodes_per_group;
                idx = (ino - 1u) % fs.sb.inodes_per_group;
                bgd = fs.bgd_buf + g * fs.bgd_size;
                it  = ((uint64_t)((fs.bgd_size>=64u)?(((uint32_t)bgd[0x28])
                       |((uint32_t)bgd[0x29]<<8)|((uint32_t)bgd[0x2A]<<16)
                       |((uint32_t)bgd[0x2B]<<24)) : 0u) << 32)
                    | (((uint32_t)bgd[0x08])|((uint32_t)bgd[0x09]<<8)
                       |((uint32_t)bgd[0x0A]<<16)|((uint32_t)bgd[0x0B]<<24));
                fb  = it + (uint64_t)idx * fs.sb.inode_size / fs.sb.block_size;
                off = (uint32_t)((uint64_t)idx * fs.sb.inode_size % fs.sb.block_size);
                rc  = ext4_fs_read_block(&fs, fb, inblk);
                ASSERT(rc == 0, "post-move inode read");
                {
                    uint32_t nl = (uint32_t)inblk[off+0x1A] | ((uint32_t)inblk[off+0x1B]<<8);
                    if (phase == 0) post_root_nlinks = nl;
                    else            post_xsub_nlinks = nl;
                }
            }
            ASSERT(post_root_nlinks + 1u == pre_root_nlinks,
                   "root nlinks pre=%u post=%u (expected post=pre-1)",
                   (unsigned)pre_root_nlinks, (unsigned)post_root_nlinks);
            ASSERT(post_xsub_nlinks == pre_xsub_nlinks + 1u,
                   "xsub nlinks pre=%u post=%u (expected post=pre+1)",
                   (unsigned)pre_xsub_nlinks, (unsigned)post_xsub_nlinks);
        }

        /* --- 9. Second hop: /xsub/xdir1 -> /xdir2/sub (rename mid-move) --- */
        rc = ext4_rename_xdir(&fs, sub_ino, xdir1_ino, xdir2_ino,
                              "sub", 3u, now+80u, err, sizeof err);
        ASSERT(rc == 0, "xdir DIR rename hop2: '%s'", err);
        ASSERT(ext4_path_lookup(&fs, "/xsub/xdir1") == 0,
               "/xsub/xdir1 still found after second move");
        ASSERT(ext4_path_lookup(&fs, "/xdir2/sub") == xdir1_ino,
               "/xdir2/sub missing — second move broke");
        ASSERT(ext4_path_lookup(&fs, "/xdir2/sub/leaf.txt") == leaf_ino,
               "/xdir2/sub/leaf.txt — leaf lost across two hops");

        /* --- 10. Cycle: refuse moving xdir2 into xdir2/sub (its own descendant) --- */
        rc = ext4_rename_xdir(&fs, 2u, xdir2_ino, xdir1_ino,
                              "xdir2cycle", 10u, now+90u, err, sizeof err);
        ASSERT(rc != 0, "cycle move (xdir2 -> /xdir2/sub) must be refused");
    }

    ASSERT(fs.jbd.start == 0, "journal must be clean");

    if (failures == 0) {
        printf("host_rename_xdir_test: all asserts passed\n");
        return 0;
    }
done:
    fprintf(stderr, "host_rename_xdir_test: %d FAILURE(S)\n", failures);
    return 1;
}
