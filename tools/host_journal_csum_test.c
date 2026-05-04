/* Per-tag data-block CRC regression: a torn write inside a journaled
 * data block (descriptor + commit blocks intact, only the data was lost
 * mid-flight) must be caught by replay rather than silently applied to
 * the fs.
 *
 * Flow:
 *   1. Copy the CSUM_V2 fixture (journal-csum.img) to a working file.
 *   2. Mount it writable. The unmodified fixture must mount cleanly —
 *      this is the false-positive sanity check on the new verify path.
 *      We also capture the journal extent layout so step 3 knows where
 *      on disk the journaled data block lives.
 *   3. Copy the fixture again (the writable mount in step 2 checkpointed
 *      it, leaving an empty journal — we need a fresh dirty journal).
 *   4. Open the fresh copy, raw-bdev-write a flipped byte into the
 *      journaled data block, close.
 *   5. Re-mount writable. ext4_fs_open must refuse with rc == -6
 *      (ext4_journal_replay propagating walk_log's rc=-8 from the
 *      per-tag csum mismatch in iterate_descriptor_tags).
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

static int read_expect(const char *path, uint32_t *journal_blk) {
    FILE *f = fopen(path, "r");
    char  line[128];
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        unsigned v;
        if (sscanf(line, "journal_blk=%u", &v) == 1) *journal_blk = v;
    }
    fclose(f);
    return 0;
}

static int cp_file(const char *src, const char *dst) {
    FILE *fsrc, *fdst;
    char  buf[8192];
    size_t n;
    fsrc = fopen(src, "rb"); if (!fsrc) return -1;
    fdst = fopen(dst, "wb"); if (!fdst) { fclose(fsrc); return -2; }
    while ((n = fread(buf, 1, sizeof buf, fsrc)) > 0) {
        if (fwrite(buf, 1, n, fdst) != n) { fclose(fsrc); fclose(fdst); return -3; }
    }
    fclose(fsrc); fclose(fdst);
    return 0;
}

int main(int argc, char **argv) {
    const char *src_path    = (argc > 1) ? argv[1] : "tests/images/journal-csum.img";
    const char *expect_path = (argc > 2) ? argv[2] : "tests/images/journal-csum.expect";
    const char *work_path   = (argc > 3) ? argv[3] : "tests/images/journal-csum-bad-test.img";
    uint32_t journal_blk_expect = 0;
    uint64_t phys_block = 0;
    uint32_t sectors_per_block = 0;
    uint32_t corrupt_sector = 0;
    uint32_t block_size = 0;
    static uint8_t data[4096];
    static struct ext4_fs fs1, fs2;
    struct blockdev *bd;
    int rc;

    if (read_expect(expect_path, &journal_blk_expect) != 0
        || journal_blk_expect == 0) {
        fprintf(stderr, "FAIL: couldn't read journal_blk from %s\n", expect_path);
        return 2;
    }

    /* Step 1+2: prime working copy, mount writable, capture extent layout.
     * The unmodified fixture must mount clean — confirms the new verify
     * code doesn't reject good journals. */
    rc = cp_file(src_path, work_path);
    ASSERT(rc == 0, "cp_file priming rc=%d", rc);

    bd = file_bdev_open_rw(work_path);
    ASSERT(bd != NULL, "file_bdev_open_rw failed");
    if (!bd) return 1;

    rc = ext4_fs_open(&fs1, bd, 0);
    ASSERT(rc == 0, "unmodified CSUM_V2 fixture must mount, got rc=%d", rc);
    if (rc != 0) { file_bdev_close(bd); return 1; }

    ASSERT(fs1.jbd.extent_count >= 1, "journal must have at least one extent");
    ASSERT(fs1.jbd.csum_v2 || fs1.jbd.csum_v3,
           "fixture must have CSUM_V2 or V3 to exercise per-tag verify");
    /* journal_blk is an index into the log (0..maxlen-1). Translate via
     * the extent layout — same logic as ext4_journal_read_log_block.
     * Real journals on ext4 disks are NOT a single contiguous extent
     * (this fixture's journal has 3), so the loop is necessary. */
    {
        uint32_t i;
        phys_block = 0;
        for (i = 0; i < fs1.jbd.extent_count; i++) {
            const struct ext4_jbd_extent *e = &fs1.jbd.extents[i];
            if (journal_blk_expect >= e->logical
                && journal_blk_expect < e->logical + e->length) {
                phys_block = e->physical + (uint64_t)(journal_blk_expect - e->logical);
                break;
            }
        }
    }
    ASSERT(phys_block != 0, "couldn't translate journal_blk=%u to phys", journal_blk_expect);
    block_size        = fs1.sb.block_size;
    sectors_per_block = block_size / fs1.bd->sector_size;
    corrupt_sector    = (uint32_t)(phys_block * sectors_per_block);
    file_bdev_close(bd);

    /* Step 3+4: reset working copy (the prior writable mount checkpointed
     * the journal — jsb.start = 0). Open fresh, flip a byte in the
     * journaled data block via raw bdev_write. */
    rc = cp_file(src_path, work_path);
    ASSERT(rc == 0, "cp_file refresh rc=%d", rc);

    bd = file_bdev_open_rw(work_path);
    ASSERT(bd != NULL, "file_bdev_open_rw post-refresh failed");
    if (!bd) return 1;

    rc = bdev_read(bd, corrupt_sector, sectors_per_block, data);
    ASSERT(rc == 0, "raw read of journaled data block rc=%d", rc);
    /* Flip a byte well past the first 4 (so this is not also confused
     * with the ESCAPE flag's magic-zeroing convention). The tag csum
     * covers the entire block, so any single-byte flip will mismatch. */
    data[64] ^= 0xFF;
    rc = bdev_write(bd, corrupt_sector, sectors_per_block, data);
    ASSERT(rc == 0, "raw write of corrupted block rc=%d", rc);
    file_bdev_close(bd);

    /* Step 5: re-mount. The descriptor + commit blocks are still intact
     * (we didn't touch them), but the per-tag csum no longer matches the
     * data on disk. iterate_descriptor_tags returns -8; walk_log
     * propagates; ext4_journal_replay returns -8; ext4_fs_open returns
     * -6. Without the new verify, this would silently FLUSH garbage onto
     * the fs and report success. */
    bd = file_bdev_open_rw(work_path);
    ASSERT(bd != NULL, "file_bdev_open_rw remount failed");
    if (!bd) return 1;

    rc = ext4_fs_open(&fs2, bd, 0);
    ASSERT(rc == -6, "corrupted-data mount must fail rc=-6, got rc=%d", rc);
    file_bdev_close(bd);

    if (failures == 0) {
        printf("host_journal_csum_test: per-tag CSUM_V2 verify caught "
               "torn write at journal_blk=%u (phys=%llu)\n",
               journal_blk_expect, (unsigned long long)phys_block);
        return 0;
    }
    fprintf(stderr, "host_journal_csum_test: %d FAILURE(S)\n", failures);
    return 1;
}
