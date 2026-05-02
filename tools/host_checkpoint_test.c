/* Host-side test for ext4_journal_checkpoint (hard-flush).
 *
 * Takes a working copy of the dirty-journal fixture, opens it via the
 * writable file blockdev, and asserts that ext4_fs_open performs the
 * hard-flush:
 *
 *   1. Replay map is consumed (replay_active == 0, replay_count == 0).
 *   2. journal sb on disk has s_start == 0.
 *   3. FS sb feature_incompat no longer has RECOVER (0x4) set.
 *   4. The previously-journaled block, read via raw bdev_read (NOT the
 *      replay redirect), now contains the journaled bytes — proving the
 *      flush actually wrote to disk.
 *
 * Working copy is made by the Makefile via cp; this test mutates it. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "blockdev/blockdev.h"
#include "blockdev/file_bdev.h"
#include "ext4/superblock.h"
#include "ext4/features.h"
#include "ext4/fs.h"
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

static int read_expect(const char *path, uint32_t *fs_block, uint32_t *journal_byte0) {
    FILE *f = fopen(path, "r");
    char  line[128];
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        unsigned v;
        if      (sscanf(line, "fs_block=%u",       &v) == 1) *fs_block      = v;
        else if (sscanf(line, "journal_byte0=0x%x",&v) == 1) *journal_byte0 = v;
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    const char *img    = (argc > 1) ? argv[1] : "tests/images/journal-flush.img";
    const char *expect = (argc > 2) ? argv[2] : "tests/images/journal.expect";
    struct blockdev   *bd;
    static struct ext4_fs fs;
    uint32_t fs_block_expect = 0;
    uint32_t journal_byte0   = 0;
    int rc;

    if (read_expect(expect, &fs_block_expect, &journal_byte0) != 0
        || fs_block_expect == 0) {
        fprintf(stderr, "FAIL: couldn't parse %s\n", expect);
        return 2;
    }

    /* Mount writable — fs_open should hard-flush. */
    bd = file_bdev_open_rw(img);
    ASSERT(bd != NULL, "couldn't open %s rw", img);
    if (!bd) return 1;
    ASSERT(bdev_writable(bd) == 1, "expected bdev to report writable");

    rc = ext4_fs_open(&fs, bd, 0);
    ASSERT(rc == 0, "ext4_fs_open rc=%d", rc);

    /* Post-flush state. */
    ASSERT(fs.jbd.replay_active == 0, "replay_active should be 0 after flush, got %u",
           (unsigned)fs.jbd.replay_active);
    ASSERT(fs.jbd.replay_count == 0,  "replay_count should be 0 after flush, got %u",
           (unsigned)fs.jbd.replay_count);
    ASSERT(fs.jbd.start == 0,         "jsb.start should be 0 after flush, got %u",
           (unsigned)fs.jbd.start);
    ASSERT((fs.sb.feature_incompat & 0x4u) == 0,
           "RECOVER should be cleared on fs.sb after flush, feature_incompat=0x%x",
           (unsigned)fs.sb.feature_incompat);

    /* Raw read of the previously-journaled block — must now reflect the
     * journaled bytes since they were flushed to disk. */
    {
        static uint8_t raw[4096];
        uint32_t byte    = (uint32_t)((uint64_t)fs_block_expect * fs.sb.block_size);
        uint32_t sector  = byte / fs.bd->sector_size;
        uint32_t sectors = fs.sb.block_size / fs.bd->sector_size;
        rc = bdev_read(fs.bd, sector, sectors, raw);
        ASSERT(rc == 0, "raw bdev_read rc=%d", rc);
        ASSERT(raw[0] == (uint8_t)journal_byte0,
               "post-flush raw byte0 expected 0x%02x (journaled), got 0x%02x",
               journal_byte0, raw[0]);
    }

    file_bdev_close(bd);

    /* Re-open and re-mount: previously-dirty journal must now mount as clean. */
    bd = file_bdev_open(img);
    ASSERT(bd != NULL, "couldn't reopen %s", img);
    if (!bd) return 1;
    rc = ext4_fs_open(&fs, bd, 0);
    ASSERT(rc == 0, "second ext4_fs_open rc=%d", rc);
    ASSERT(fs.jbd.replay_active == 0, "second mount should see clean log");
    ASSERT(fs.jbd.start == 0,         "jsb.start still non-zero on second mount");

    if (failures == 0) {
        printf("host_checkpoint_test: all asserts passed (flushed fs_block=%u, "
               "RECOVER cleared, second mount is clean)\n",
               fs_block_expect);
        return 0;
    }
    fprintf(stderr, "host_checkpoint_test: %d FAILURE(S)\n", failures);
    return 1;
}
