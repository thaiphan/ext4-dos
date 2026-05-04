/* Host-side test for the streaming-flush replay path.
 *
 * Mounts a fixture whose dirty journal contains > EXT4_JBD_REPLAY_MAP_MAX
 * unique fs_blocks (built by scripts/mkfixture-journal-large.py). The
 * map-based replay would overflow and silently abort with stale on-disk
 * state; the streaming path applies each tag inline during the walk and
 * has no cap.
 *
 * Asserts:
 *   1. ext4_fs_open succeeds.
 *   2. replay_active == 0 (streaming cleared its own state).
 *   3. jsb.start == 0 in fs_open's view.
 *   4. RECOVER cleared on the FS SB on disk (re-read directly).
 *   5. The first targeted fs_block on disk contains the journaled bytes
 *      (byte 0 = 0x00, byte 1 = 0x5A) — proves streaming actually wrote
 *      the journal data through to the fs_block locations.
 *
 * Mutates a working copy created by the Makefile via cp. */
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

#define SB_OFF                 1024u
#define SB_LEN                 1024u
#define SB_FEAT_INCOMPAT_OFF   0x60u
#define SB_INCOMPAT_RECOVER    0x4u

static uint32_t le32_read(const uint8_t *p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static int read_sb_feat_incompat(const char *path, uint32_t *out) {
    FILE     *f;
    uint8_t   sb[SB_LEN];
    f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, SB_OFF, SEEK_SET) != 0)   { fclose(f); return -2; }
    if (fread(sb, 1, SB_LEN, f) != SB_LEN) { fclose(f); return -3; }
    fclose(f);
    *out = le32_read(sb + SB_FEAT_INCOMPAT_OFF);
    return 0;
}

static int read_expect(const char *path, uint32_t *first_target,
                       unsigned *byte0, unsigned *byte1, unsigned *n_tags) {
    FILE *f = fopen(path, "r");
    char  line[128];
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        unsigned v;
        if      (sscanf(line, "first_target_fs_block=%u", &v) == 1) *first_target = v;
        else if (sscanf(line, "first_target_byte0=0x%x",  &v) == 1) *byte0        = v;
        else if (sscanf(line, "first_target_byte1=0x%x",  &v) == 1) *byte1        = v;
        else if (sscanf(line, "n_tags=%u",                &v) == 1) *n_tags       = v;
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    const char *img    = (argc > 1) ? argv[1] : "tests/images/journal-large-test.img";
    const char *expect = (argc > 2) ? argv[2] : "tests/images/journal-large.expect";
    struct blockdev   *bd;
    static struct ext4_fs fs;
    uint32_t first_target = 0;
    unsigned byte0 = 0, byte1 = 0, n_tags = 0;
    uint32_t feat_incompat;
    int rc;

    if (read_expect(expect, &first_target, &byte0, &byte1, &n_tags) != 0
        || first_target == 0 || n_tags == 0) {
        fprintf(stderr, "FAIL: couldn't parse %s\n", expect);
        return 2;
    }

    /* Pre-mount: confirm RECOVER was set on the fixture. */
    rc = read_sb_feat_incompat(img, &feat_incompat);
    ASSERT(rc == 0, "pre-mount sb read rc=%d", rc);
    ASSERT((feat_incompat & SB_INCOMPAT_RECOVER) != 0,
           "fixture should have RECOVER set, feature_incompat=0x%x",
           (unsigned)feat_incompat);

    /* Mount writable — streaming flush should run. */
    bd = file_bdev_open_rw(img);
    ASSERT(bd != NULL, "couldn't open %s rw", img);
    if (!bd) return 1;
    ASSERT(bdev_writable(bd) == 1, "expected bdev to report writable");

    rc = ext4_fs_open(&fs, bd, 0);
    ASSERT(rc == 0, "ext4_fs_open rc=%d", rc);

    /* Post-flush state — replay map never built, FS clean. */
    ASSERT(fs.jbd.replay_active == 0,
           "replay_active should be 0 after streaming, got %u",
           (unsigned)fs.jbd.replay_active);
    ASSERT(fs.jbd.replay_count == 0,
           "replay_count should be 0 after streaming, got %u",
           (unsigned)fs.jbd.replay_count);
    ASSERT(fs.jbd.start == 0,
           "jsb.start should be 0 after streaming, got %u",
           (unsigned)fs.jbd.start);
    ASSERT((fs.sb.feature_incompat & SB_INCOMPAT_RECOVER) == 0,
           "RECOVER should be cleared on fs.sb, feature_incompat=0x%x",
           (unsigned)fs.sb.feature_incompat);

    /* Raw read of the first targeted fs_block — must reflect the journaled
     * payload (byte0=0x00, byte1=0x5A) since streaming wrote it. */
    {
        static uint8_t raw[4096];
        uint32_t byte    = (uint32_t)((uint64_t)first_target * fs.sb.block_size);
        uint32_t sector  = byte / fs.bd->sector_size;
        uint32_t sectors = fs.sb.block_size / fs.bd->sector_size;
        rc = bdev_read(fs.bd, sector, sectors, raw);
        ASSERT(rc == 0, "raw bdev_read rc=%d", rc);
        ASSERT(raw[0] == (uint8_t)byte0,
               "post-flush byte0 expected 0x%02x, got 0x%02x", byte0, raw[0]);
        ASSERT(raw[1] == (uint8_t)byte1,
               "post-flush byte1 expected 0x%02x, got 0x%02x", byte1, raw[1]);
    }

    file_bdev_close(bd);

    /* Re-read fs SB on disk. RECOVER must persist as cleared. */
    rc = read_sb_feat_incompat(img, &feat_incompat);
    ASSERT(rc == 0, "post-mount sb read rc=%d", rc);
    ASSERT((feat_incompat & SB_INCOMPAT_RECOVER) == 0,
           "RECOVER still set on disk: feature_incompat=0x%x",
           (unsigned)feat_incompat);

    /* Re-mount: previously-dirty journal is now clean. */
    bd = file_bdev_open(img);
    ASSERT(bd != NULL, "couldn't reopen %s", img);
    if (!bd) return 1;
    rc = ext4_fs_open(&fs, bd, 0);
    ASSERT(rc == 0, "second ext4_fs_open rc=%d", rc);
    ASSERT(fs.jbd.replay_active == 0, "second mount should see clean log");
    ASSERT(fs.jbd.start == 0,         "jsb.start non-zero on second mount");

    if (failures == 0) {
        printf("host_journal_streaming_test: all asserts passed "
               "(streamed %u tags past the %u-entry cap, fs_block=%u carries journaled bytes)\n",
               n_tags, 64u, first_target);
        return 0;
    }
    fprintf(stderr, "host_journal_streaming_test: %d FAILURE(S)\n", failures);
    return 1;
}
