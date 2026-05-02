/* Host-side test for journal replay.
 *
 * Loads tests/images/journal.img — a raw ext4 FS with one forged
 * transaction in its log (built by scripts/mkfixture-journal.py) — and
 * asserts that the replay walker built a map containing exactly the
 * forged entry.
 *
 * Sidecar journal.expect documents what the walker should have found:
 *   fs_block / journal_blk / journaled-byte / on-disk-byte
 *
 * Run as part of `make host-test`. */
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

static int read_expect(const char *path, uint32_t *fs_block,
                       uint32_t *journal_blk, uint32_t *on_disk_byte0,
                       uint32_t *journal_byte0) {
    FILE *f = fopen(path, "r");
    char  line[128];
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        unsigned v;
        if      (sscanf(line, "fs_block=%u",       &v) == 1) *fs_block       = v;
        else if (sscanf(line, "journal_blk=%u",    &v) == 1) *journal_blk    = v;
        else if (sscanf(line, "on_disk_byte0=0x%x",&v) == 1) *on_disk_byte0  = v;
        else if (sscanf(line, "journal_byte0=0x%x",&v) == 1) *journal_byte0  = v;
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    const char       *img_path     = (argc > 1) ? argv[1] : "tests/images/journal.img";
    const char       *expect_path  = (argc > 2) ? argv[2] : "tests/images/journal.expect";
    struct blockdev  *bd;
    static struct ext4_fs fs;
    uint32_t          fs_block_expect    = 0;
    uint32_t          journal_blk_expect = 0;
    uint32_t          on_disk_byte0      = 0;
    uint32_t          journal_byte0      = 0;
    uint32_t          jblk;
    uint8_t           is_escape;
    int               hit, rc;

    if (read_expect(expect_path, &fs_block_expect, &journal_blk_expect,
                    &on_disk_byte0, &journal_byte0) != 0) {
        fprintf(stderr, "FAIL: couldn't read %s — did you run scripts/mkfixture-journal.py?\n",
                expect_path);
        return 2;
    }

    bd = file_bdev_open(img_path);
    ASSERT(bd != NULL, "couldn't open %s", img_path);
    if (!bd) return 1;

    /* Raw FS — no MBR. partition_lba = 0. */
    rc = ext4_fs_open(&fs, bd, 0);
    ASSERT(rc == 0, "ext4_fs_open rc=%d", rc);

    /* RECOVER is set in the forged image, but features_check_supported
     * already allows it (see EXT4_V1_INCOMPAT_SUPPORTED). */
    ASSERT(fs.jbd.present == 1,        "journal must be present");
    ASSERT(fs.jbd.start != 0,          "forged journal should have non-zero start");
    ASSERT(fs.jbd.replay_active == 1,  "replay must be active for dirty journal");
    ASSERT(fs.jbd.replay_count == 1,   "expected 1 entry in replay map, got %u",
                                       (unsigned)fs.jbd.replay_count);

    /* The forged transaction has no revokes. */
    ASSERT(fs.jbd.revoke_count == 0,   "expected 0 revokes, got %u",
                                       (unsigned)fs.jbd.revoke_count);

    /* The replay map should redirect fs_block -> journal_blk. */
    hit = ext4_journal_lookup(&fs, (uint64_t)fs_block_expect, &jblk, &is_escape);
    ASSERT(hit == 1,                   "lookup of fs_block=%u missed", fs_block_expect);
    ASSERT(jblk == journal_blk_expect, "wrong jblk: expected %u, got %u",
                                       journal_blk_expect, jblk);
    ASSERT(is_escape == 0,             "tag should not be escaped");

    /* End-to-end: ext4_fs_read_block should return the journaled bytes,
     * not the on-disk bytes. The forged data block has its first byte
     * XORed; both bytes are recorded in the expect file. */
    {
        static uint8_t blk[4096];
        rc = ext4_fs_read_block(&fs, (uint64_t)fs_block_expect, blk);
        ASSERT(rc == 0, "ext4_fs_read_block rc=%d", rc);
        ASSERT(blk[0] == (uint8_t)journal_byte0,
               "post-replay byte0 expected 0x%02x (journaled), got 0x%02x "
               "(on-disk would be 0x%02x)",
               journal_byte0, blk[0], on_disk_byte0);
    }

    /* Bypass: a raw bdev_read of the same block (no replay) should
     * return the on-disk content. This proves the redirect is actually
     * happening, not just coincidence. */
    {
        static uint8_t raw[4096];
        uint32_t byte    = (uint32_t)((uint64_t)fs_block_expect * fs.sb.block_size);
        uint32_t sector  = byte / fs.bd->sector_size;
        uint32_t sectors = fs.sb.block_size / fs.bd->sector_size;
        rc = bdev_read(fs.bd, sector, sectors, raw);
        ASSERT(rc == 0, "raw bdev_read rc=%d", rc);
        ASSERT(raw[0] == (uint8_t)on_disk_byte0,
               "raw read byte0 expected 0x%02x (on-disk), got 0x%02x",
               on_disk_byte0, raw[0]);
    }

    if (failures == 0) {
        printf("host_journal_test: all asserts passed (replay redirects "
               "fs_block=%u -> journal_blk=%u)\n",
               fs_block_expect, journal_blk_expect);
        return 0;
    }
    fprintf(stderr, "host_journal_test: %d FAILURE(S)\n", failures);
    return 1;
}
