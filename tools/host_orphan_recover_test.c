/* Host-side test for the orphan-RECOVER cleanup path.
 *
 * Simulates a "crash mid-commit before jsb.start was bumped":
 *   1. Open a clean fixture image at byte level.
 *   2. Force RECOVER (feature_incompat bit 0x4) on the FS superblock,
 *      recomputing the SB checksum if metadata_csum is enabled.
 *   3. Mount writable via ext4_fs_open. Replay walks an empty log
 *      (jsb.start == 0) so no replay map is built — but RECOVER must
 *      still be cleared because we do mean to leave the FS clean.
 *   4. Re-mount and verify RECOVER stays cleared and the SB checksum
 *      survived the rewrite.
 *
 * Without the orphan-RECOVER fix in ext4_journal_checkpoint, RECOVER
 * stays set on disk after step 3 and e2fsck -fn flags the FS as needing
 * recovery on every subsequent boot. */
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
#include "util/crc32c.h"

static int failures = 0;
#define ASSERT(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

/* SB layout (offsets within the 1 KiB superblock) — matches what
 * journal.c:update_fs_sb_recover writes. */
#define SB_OFF                 1024u
#define SB_LEN                 1024u
#define SB_FEAT_INCOMPAT_OFF   0x60u
#define SB_FEAT_RO_COMPAT_OFF  0x64u
#define SB_CHECKSUM_OFF        0x3FCu  /* crc32c covers bytes 0..0x3FB */
#define SB_INCOMPAT_RECOVER    0x4u
#define SB_ROCOMPAT_METADATA_CSUM 0x400u

static uint32_t le32_read(const uint8_t *p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}
static void le32_write(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t)(v >>  8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Stamp RECOVER on the on-disk FS SB and recompute checksum if needed.
 * Returns 0 on success, nonzero on I/O error. */
static int stamp_recover(const char *img_path) {
    FILE     *f;
    uint8_t   sb[SB_LEN];
    uint32_t  feat_incompat, feat_ro_compat;

    f = fopen(img_path, "r+b");
    if (!f) return -1;
    if (fseek(f, SB_OFF, SEEK_SET) != 0)         { fclose(f); return -2; }
    if (fread(sb, 1, SB_LEN, f) != SB_LEN)       { fclose(f); return -3; }

    feat_incompat  = le32_read(sb + SB_FEAT_INCOMPAT_OFF);
    feat_ro_compat = le32_read(sb + SB_FEAT_RO_COMPAT_OFF);

    feat_incompat |= SB_INCOMPAT_RECOVER;
    le32_write(sb + SB_FEAT_INCOMPAT_OFF, feat_incompat);

    if (feat_ro_compat & SB_ROCOMPAT_METADATA_CSUM) {
        uint32_t csum;
        memset(sb + SB_CHECKSUM_OFF, 0, 4);
        csum = crc32c(CRC32C_INIT, sb, SB_CHECKSUM_OFF);
        le32_write(sb + SB_CHECKSUM_OFF, csum);
    }

    if (fseek(f, SB_OFF, SEEK_SET) != 0)         { fclose(f); return -4; }
    if (fwrite(sb, 1, SB_LEN, f) != SB_LEN)      { fclose(f); return -5; }
    fclose(f);
    return 0;
}

static int read_recover_bit_on_disk(const char *img_path, uint32_t *out_feat_incompat) {
    FILE    *f;
    uint8_t  sb[SB_LEN];
    f = fopen(img_path, "rb");
    if (!f) return -1;
    if (fseek(f, SB_OFF, SEEK_SET) != 0)   { fclose(f); return -2; }
    if (fread(sb, 1, SB_LEN, f) != SB_LEN) { fclose(f); return -3; }
    fclose(f);
    *out_feat_incompat = le32_read(sb + SB_FEAT_INCOMPAT_OFF);
    return 0;
}

int main(int argc, char **argv) {
    const char *img = (argc > 1) ? argv[1] : "tests/images/orphan-recover-test.img";
    struct blockdev   *bd;
    static struct ext4_fs fs;
    uint32_t feat_incompat;
    int rc;

    /* 1. Stamp RECOVER on the on-disk SB to simulate a partial commit. */
    rc = stamp_recover(img);
    ASSERT(rc == 0, "stamp_recover rc=%d", rc);
    if (rc) return 1;

    rc = read_recover_bit_on_disk(img, &feat_incompat);
    ASSERT(rc == 0, "read after stamp rc=%d", rc);
    ASSERT((feat_incompat & SB_INCOMPAT_RECOVER) != 0,
           "stamp didn't take: feature_incompat=0x%x", (unsigned)feat_incompat);

    /* 2. Mount writable. ext4_fs_open should clear the orphan RECOVER
     * even though the journal log has nothing to replay (jsb.start=0). */
    bd = file_bdev_open_rw(img);
    ASSERT(bd != NULL, "couldn't open %s rw", img);
    if (!bd) return 1;
    ASSERT(bdev_writable(bd) == 1, "expected bdev to report writable");

    rc = ext4_fs_open(&fs, bd, 0);
    ASSERT(rc == 0, "ext4_fs_open rc=%d", rc);

    ASSERT(fs.jbd.replay_active == 0,
           "replay_active should be 0 (clean log), got %u",
           (unsigned)fs.jbd.replay_active);
    ASSERT((fs.sb.feature_incompat & SB_INCOMPAT_RECOVER) == 0,
           "RECOVER should be cleared on fs.sb after mount, feature_incompat=0x%x",
           (unsigned)fs.sb.feature_incompat);

    file_bdev_close(bd);

    /* 3. Re-read the on-disk SB. RECOVER must be cleared there too
     * (the in-memory clear in ext4_journal_checkpoint must be backed by
     * a real disk write). */
    rc = read_recover_bit_on_disk(img, &feat_incompat);
    ASSERT(rc == 0, "post-mount read rc=%d", rc);
    ASSERT((feat_incompat & SB_INCOMPAT_RECOVER) == 0,
           "RECOVER still set on disk after writable mount: feature_incompat=0x%x",
           (unsigned)feat_incompat);

    /* 4. Mount again (read-only is fine) and confirm clean state survives
     * — proxy for "next boot doesn't think the FS needs recovery." */
    bd = file_bdev_open(img);
    ASSERT(bd != NULL, "couldn't reopen %s", img);
    if (!bd) return 1;
    rc = ext4_fs_open(&fs, bd, 0);
    ASSERT(rc == 0, "second ext4_fs_open rc=%d", rc);
    ASSERT((fs.sb.feature_incompat & SB_INCOMPAT_RECOVER) == 0,
           "RECOVER reappeared on second mount, feature_incompat=0x%x",
           (unsigned)fs.sb.feature_incompat);

    file_bdev_close(bd);

    if (failures == 0) {
        printf("host_orphan_recover_test: all asserts passed "
               "(stamped RECOVER, mount cleared it on disk, second mount stayed clean)\n");
        return 0;
    }
    fprintf(stderr, "host_orphan_recover_test: %d FAILURE(S)\n", failures);
    return 1;
}
