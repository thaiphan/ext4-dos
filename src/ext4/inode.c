#include "inode.h"
#include "fs.h"
#include "../blockdev/blockdev.h"
#include "../util/endian.h"
#include <string.h>

int ext4_inode_read(struct ext4_fs *fs, uint32_t ino, struct ext4_inode *out) {
    uint32_t       group;
    uint32_t       index_in_group;
    const uint8_t *bgd;
    uint32_t       inode_table_lo;
    uint32_t       inode_table_hi;
    uint64_t       inode_table_block;
    uint64_t       inode_byte;
    uint64_t       inode_sector;
    uint32_t       offset_in_sector;
    uint32_t       sector_size;
    uint32_t       end_byte;
    uint32_t       sectors_to_read;
    uint8_t        buf[1024];
    const uint8_t *raw;
    uint32_t       size_lo;
    uint32_t       size_hi;
    int            rc;

    if (ino == 0) return -1;

    group          = (ino - 1u) / fs->sb.inodes_per_group;
    index_in_group = (ino - 1u) % fs->sb.inodes_per_group;
    if (group >= fs->bgd_count) return -2;

    bgd = fs->bgd_buf + (uint32_t)group * fs->bgd_size;
    inode_table_lo = le32(bgd + 0x08);
    inode_table_hi = (fs->bgd_size >= 64u) ? le32(bgd + 0x28) : 0u;
    inode_table_block = ((uint64_t)inode_table_hi << 32) | inode_table_lo;

    inode_byte = inode_table_block * (uint64_t)fs->sb.block_size
               + (uint64_t)index_in_group * fs->sb.inode_size;

    sector_size      = fs->bd->sector_size;
    inode_sector     = fs->partition_lba + inode_byte / sector_size;
    offset_in_sector = (uint32_t)(inode_byte % sector_size);

    if (fs->sb.inode_size > 1024u) return -3;
    end_byte        = offset_in_sector + fs->sb.inode_size;
    sectors_to_read = (end_byte + sector_size - 1u) / sector_size;
    if (sectors_to_read * sector_size > sizeof buf) return -4;

    rc = bdev_read(fs->bd, inode_sector, sectors_to_read, buf);
    if (rc) return -5;

    raw = buf + offset_in_sector;
    out->mode  = le16(raw + 0x00);
    out->flags = le32(raw + 0x20);
    memcpy(out->i_block, raw + 0x28, 60);

    size_lo = le32(raw + 0x04);
    size_hi = le32(raw + 0x6C);
    out->size = ((uint64_t)size_hi << 32) | size_lo;

    return 0;
}
