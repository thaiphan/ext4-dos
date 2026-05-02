/* Host-side test for ext4_file_write_block.
 *
 * Mounts a writable working copy of the write fixture, finds /target.txt,
 * writes one block of 'B' over its existing 'A' content, and asserts:
 *   - mtime advanced
 *   - read-back returns 'B'-block
 *   - journal is clean (start=0) after — commit + checkpoint round-tripped
 *   - the second file (/control.txt) is untouched
 *
 * Mutates a working copy created by the Makefile via cp. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    const char        *img = (argc > 1) ? argv[1] : "tests/images/write-test.img";
    struct blockdev   *bd;
    static struct ext4_fs fs;
    static struct ext4_inode inode;
    static uint8_t     new_block[1024];
    static uint8_t     verify_block[1024];
    char               err[128];
    uint32_t           target_ino, control_ino;
    uint32_t           orig_mtime, now;
    int                rc;

    bd = file_bdev_open_rw(img);
    ASSERT(bd != NULL, "couldn't open %s rw", img);
    if (!bd) return 1;

    rc = ext4_fs_open(&fs, bd, 0);
    ASSERT(rc == 0, "ext4_fs_open rc=%d", rc);

    /* Sanity: clean fixture (no replay needed). */
    ASSERT(fs.jbd.replay_active == 0, "fixture should be clean");
    ASSERT(fs.jbd.start         == 0, "fixture jsb.start should be 0");

    target_ino = ext4_path_lookup(&fs, "/target.txt");
    ASSERT(target_ino != 0, "/target.txt not found");
    if (target_ino == 0) return 1;

    control_ino = ext4_path_lookup(&fs, "/control.txt");
    ASSERT(control_ino != 0, "/control.txt not found");

    rc = ext4_inode_read(&fs, target_ino, &inode);
    ASSERT(rc == 0, "ext4_inode_read rc=%d", rc);
    orig_mtime = inode.mtime;

    /* Write one block of 'B' over /target.txt's first (only) block. */
    memset(new_block, 'B', sizeof new_block);
    /* Pick a now value distinct from orig_mtime so the bump is observable. */
    now = orig_mtime + 1u;
    while (now == orig_mtime) now++;

    rc = ext4_file_write_block(&fs, &inode, target_ino, /*logical=*/0,
                               new_block, now, err, sizeof err);
    ASSERT(rc == 0, "ext4_file_write_block rc=%d err='%s'", rc, err);

    /* Caller's parsed inode should reflect the new mtime. */
    ASSERT(inode.mtime == now, "in-memory inode.mtime not updated (got %lu, expected %lu)",
           (unsigned long)inode.mtime, (unsigned long)now);

    /* Post-commit: journal must be clean (commit + immediate checkpoint). */
    ASSERT(fs.jbd.start == 0, "jsb.start should be 0 after commit, got %lu",
           (unsigned long)fs.jbd.start);
    ASSERT(fs.jbd.replay_active == 0, "replay_active should be 0 after commit");

    /* Read back: re-read the inode from disk and read its first block. */
    rc = ext4_inode_read(&fs, target_ino, &inode);
    ASSERT(rc == 0, "re-read inode rc=%d", rc);
    ASSERT(inode.mtime == now, "on-disk inode.mtime not persisted (got %lu, expected %lu)",
           (unsigned long)inode.mtime, (unsigned long)now);

    rc = ext4_file_read_block(&fs, &inode, /*logical=*/0, verify_block);
    ASSERT(rc == 0, "read /target.txt block rc=%d", rc);
    {
        int bad = 0;
        size_t i;
        for (i = 0; i < sizeof verify_block; i++) {
            if (verify_block[i] != 'B') { bad++; }
        }
        ASSERT(bad == 0, "/target.txt block has %d non-'B' bytes", bad);
    }

    /* Control file: should be byte-identical to before (we didn't touch it). */
    {
        struct ext4_inode cinode;
        static uint8_t cbuf[64];
        rc = ext4_inode_read(&fs, control_ino, &cinode);
        ASSERT(rc == 0, "read /control.txt inode rc=%d", rc);
        rc = ext4_file_read_head(&fs, &cinode, sizeof cbuf, cbuf);
        ASSERT(rc > 0, "read /control.txt rc=%d", rc);
        ASSERT(memcmp(cbuf, "unchanged\n", 10) == 0,
               "/control.txt was accidentally modified");
    }

    /* Extend /target.txt by exactly one block of 'C' bytes.
     * Reuses the same working copy — the post-write inode now has 1024
     * bytes; we expect a 2048-byte file with block 0 'B' and block 1
     * 'C' afterwards. */
    {
        static uint8_t ext_block[1024];
        static uint8_t verify[1024];
        uint64_t orig_size;
        memset(ext_block, 'C', sizeof ext_block);
        rc = ext4_inode_read(&fs, target_ino, &inode);
        ASSERT(rc == 0, "re-read inode for extend rc=%d", rc);
        orig_size = inode.size;

        rc = ext4_file_extend_block(&fs, &inode, target_ino, ext_block, sizeof ext_block,
                                    now + 1u, err, sizeof err);
        ASSERT(rc == 0, "ext4_file_extend_block rc=%d err='%s'", rc, err);

        ASSERT(inode.size == orig_size + 1024u,
               "inode.size after extend expected %lu, got %lu",
               (unsigned long)(orig_size + 1024u),
               (unsigned long)inode.size);

        /* Re-read inode from disk to confirm persistence. */
        rc = ext4_inode_read(&fs, target_ino, &inode);
        ASSERT(rc == 0, "re-read inode post-extend rc=%d", rc);
        ASSERT(inode.size == orig_size + 1024u,
               "on-disk inode.size after extend expected %lu, got %lu",
               (unsigned long)(orig_size + 1024u),
               (unsigned long)inode.size);

        /* Block 0 should still be 'B' (from the in-place write). */
        rc = ext4_file_read_block(&fs, &inode, 0, verify);
        ASSERT(rc == 0, "read /target.txt block 0 post-extend rc=%d", rc);
        {
            int bad = 0;
            size_t i;
            for (i = 0; i < sizeof verify; i++) if (verify[i] != 'B') bad++;
            ASSERT(bad == 0, "block 0 has %d non-'B' bytes after extend", bad);
        }

        /* Block 1 should be 'C' (the just-extended block). */
        rc = ext4_file_read_block(&fs, &inode, 1, verify);
        ASSERT(rc == 0, "read /target.txt block 1 (extended) rc=%d", rc);
        {
            int bad = 0;
            size_t i;
            for (i = 0; i < sizeof verify; i++) if (verify[i] != 'C') bad++;
            ASSERT(bad == 0, "extended block 1 has %d non-'C' bytes", bad);
        }
    }

    if (failures == 0) {
        printf("host_write_test: all asserts passed (wrote 1024B of 'B' to /target.txt, "
               "mtime %lu->%lu, journal clean post-commit)\n",
               (unsigned long)orig_mtime, (unsigned long)now);
        return 0;
    }
    fprintf(stderr, "host_write_test: %d FAILURE(S)\n", failures);
    return 1;
}
