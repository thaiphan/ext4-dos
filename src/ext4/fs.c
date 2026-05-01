#include "fs.h"
#include "../blockdev/blockdev.h"
#include "../util/endian.h"
#include <string.h>

int ext4_fs_open(struct ext4_fs *fs, struct blockdev *bd, uint64_t partition_lba) {
    uint8_t  sb_buf[1024];
    uint32_t sector_size;
    uint64_t bgd_block;
    uint64_t bgd_byte;
    uint64_t bgd_sector;
    uint32_t bgd_total_bytes;
    uint32_t sectors_to_read;
    int      rc;

    memset(fs, 0, sizeof *fs);
    fs->bd = bd;
    fs->partition_lba = partition_lba;

    /* Superblock is at byte 1024 of the partition.
     * sector_size assumed to divide 1024 cleanly (true for 512 and 1024). */
    sector_size = bd->sector_size;
    rc = bdev_read(bd, partition_lba + (1024u / sector_size),
                   1024u / sector_size, sb_buf);
    if (rc) return -1;
    if (ext4_superblock_parse(sb_buf, &fs->sb) != 0) return -2;

    /* BGD table starts at FS block 1 if block_size > 1024, else block 2. */
    bgd_block = (fs->sb.block_size > 1024u) ? 1u : 2u;

    /* Number of block groups = ceil(blocks_count / blocks_per_group). */
    {
        uint64_t bg = fs->sb.blocks_count + fs->sb.blocks_per_group - 1u;
        bg /= fs->sb.blocks_per_group;
        if (bg > 0xFFFFFFFFul) return -3;
        fs->bgd_count = (uint32_t)bg;
    }

    fs->bgd_size = (fs->sb.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? 64u : 32u;

    bgd_total_bytes = fs->bgd_count * fs->bgd_size;
    if (bgd_total_bytes > EXT4_FS_BGD_BUF_SIZE) return -4;

    bgd_byte = bgd_block * (uint64_t)fs->sb.block_size;
    bgd_sector = partition_lba + bgd_byte / sector_size;
    sectors_to_read = (bgd_total_bytes + sector_size - 1u) / sector_size;

    rc = bdev_read(bd, bgd_sector, sectors_to_read, fs->bgd_buf);
    if (rc) return -5;

    return 0;
}

void ext4_fs_close(struct ext4_fs *fs) {
    (void)fs;
    /* No dynamic state; nothing to free. */
}
