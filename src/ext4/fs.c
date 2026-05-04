#include "fs.h"
#include "journal.h"
#include "../blockdev/blockdev.h"
#include "../util/endian.h"
#include <string.h>

int ext4_fs_open(struct ext4_fs *fs, struct blockdev *bd, uint64_t partition_lba) {
    uint8_t  sb_buf[1024];
    uint32_t sector_size;
    char     jerr[64];
    int      rc;

    memset(fs, 0, sizeof *fs);
    fs->bd = bd;
    fs->partition_lba = partition_lba;
    fs->bgd_cached_block = EXT4_FS_BGD_INVALID;

    /* Superblock is at byte 1024 of the partition.
     * sector_size assumed to divide 1024 cleanly (true for 512 and 1024). */
    sector_size = bd->sector_size;
    rc = bdev_read(bd, partition_lba + (1024u / sector_size),
                   1024u / sector_size, sb_buf);
    if (rc) return -1;
    if (ext4_superblock_parse(sb_buf, &fs->sb) != 0) return -2;

    /* Number of block groups = ceil(blocks_count / blocks_per_group). */
    {
        uint64_t bg = fs->sb.blocks_count + fs->sb.blocks_per_group - 1u;
        bg /= fs->sb.blocks_per_group;
        if (bg > 0xFFFFFFFFul) return -3;
        fs->bgd_count = (uint32_t)bg;
    }

    fs->bgd_size = (fs->sb.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? 64u : 32u;

    /* BGDs are loaded on demand by ext4_fs_bgd_get — no bulk preload, no
     * cap on the BGD table size. The bgd_buf field is a single-FS-block
     * cache; bgd_cached_block tracks which fs_block is resident. Big
     * filesystems with thousands of groups work transparently — random
     * access pays one extra bdev_read per cache miss, sequential access
     * (find_free_block scanning) hits the cache for groups that share an
     * FS block. */

    /* Journal init: a parse failure does not block the mount — callers
     * can inspect fs->jbd.present. Replay failure is a different story:
     * after a partial streaming-flush or a cap-exceeded BUILD, reads
     * could mix replayed and pre-replay data. Refuse the mount in those
     * cases (RECOVER stays set on disk; a future mount retries
     * idempotently — clean Linux first if it's persistent).
     *
     * If the bdev is writable, ext4_journal_checkpoint runs after replay
     * to (a) flush any soft-replay map built on a writable read-only-mode
     * fallback, and (b) clear orphan RECOVER stuck from a prior crash-
     * mid-commit. No-op if already clean. */
    if (ext4_journal_init(fs, jerr, sizeof jerr) == 0 && fs->jbd.present) {
        if (ext4_journal_replay(fs, jerr, sizeof jerr) != 0) return -6;
        if (bdev_writable(fs->bd)) {
            (void)ext4_journal_checkpoint(fs, jerr, sizeof jerr);
        }
    }

    return 0;
}

void ext4_fs_close(struct ext4_fs *fs) {
    (void)fs;
    /* No dynamic state; nothing to free. */
}

int ext4_fs_read_block(struct ext4_fs *fs, uint64_t fs_block, void *out_buf) {
    uint32_t jblk;
    uint8_t  is_escape;
    uint32_t byte, sector, sectors;
    int      rc;

    if (ext4_journal_lookup(fs, fs_block, &jblk, &is_escape)) {
        rc = ext4_journal_read_log_block(fs, jblk, out_buf);
        if (rc) return rc;
        if (is_escape) {
            /* jbd2 zeroes the first u32 of any data block whose original
             * value was the journal magic, so the descriptor walker
             * doesn't get confused. On replay we restore it. */
            uint8_t *p = (uint8_t *)out_buf;
            p[0] = 0xC0; p[1] = 0x3B; p[2] = 0x39; p[3] = 0x98;
        }
        return 0;
    }

    byte    = (uint32_t)(fs_block * (uint64_t)fs->sb.block_size);
    sector  = (uint32_t)(fs->partition_lba + byte / fs->bd->sector_size);
    sectors = fs->sb.block_size / fs->bd->sector_size;
    return bdev_read(fs->bd, sector, sectors, out_buf);
}

/* On-demand BGD reader. The BGD table on ext4 starts at fs block 1
 * (block_size > 1024) or fs block 2 (block_size == 1024 — superblock
 * shares fs block 0 with the boot sector and lives in fs block 1).
 * For bigger filesystems the table spans many fs blocks. We read just
 * the block holding the requested group's BGD entry. */
const uint8_t *ext4_fs_bgd_get(struct ext4_fs *fs, uint32_t group) {
    uint32_t bgd_table_start;
    uint64_t bgd_byte_in_partition;
    uint64_t fs_block;
    uint32_t off_in_block;

    if (group >= fs->bgd_count) return NULL;

    bgd_table_start = (fs->sb.block_size > 1024u) ? 1u : 2u;
    bgd_byte_in_partition = (uint64_t)bgd_table_start * fs->sb.block_size
                          + (uint64_t)group * fs->bgd_size;
    fs_block     = bgd_byte_in_partition / fs->sb.block_size;
    off_in_block = (uint32_t)(bgd_byte_in_partition % fs->sb.block_size);

    if (fs->bgd_cached_block != (uint32_t)fs_block) {
        if (ext4_fs_read_block(fs, fs_block, fs->bgd_buf) != 0) {
            fs->bgd_cached_block = EXT4_FS_BGD_INVALID;
            return NULL;
        }
        fs->bgd_cached_block = (uint32_t)fs_block;
    }
    return fs->bgd_buf + off_in_block;
}

void ext4_fs_bgd_invalidate(struct ext4_fs *fs) {
    fs->bgd_cached_block = EXT4_FS_BGD_INVALID;
}
