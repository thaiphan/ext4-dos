#include "superblock.h"
#include <string.h>

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

int ext4_superblock_parse(const uint8_t *buf, struct ext4_superblock *sb) {
    sb->magic = le16(buf + 0x38);
    if (sb->magic != EXT4_SUPERBLOCK_MAGIC) {
        return -1;
    }

    sb->inodes_count       = le32(buf + 0x00);
    sb->free_inodes_count  = le32(buf + 0x10);
    sb->first_data_block   = le32(buf + 0x14);
    sb->block_size         = 1024u << le32(buf + 0x18);
    sb->blocks_per_group   = le32(buf + 0x20);
    sb->inodes_per_group   = le32(buf + 0x28);
    sb->state              = le16(buf + 0x3A);
    sb->rev_level          = le32(buf + 0x4C);
    sb->inode_size         = le16(buf + 0x58);
    sb->feature_compat     = le32(buf + 0x5C);
    sb->feature_incompat   = le32(buf + 0x60);
    sb->feature_ro_compat  = le32(buf + 0x64);

    memcpy(sb->uuid, buf + 0x68, 16);
    memcpy(sb->volume_name, buf + 0x78, 16);
    sb->volume_name[16] = '\0';
    memcpy(sb->last_mounted, buf + 0x88, 64);
    sb->last_mounted[64] = '\0';

    {
        uint32_t blocks_lo = le32(buf + 0x04);
        uint32_t free_lo   = le32(buf + 0x0C);
        uint32_t blocks_hi = 0, free_hi = 0;
        if (sb->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
            blocks_hi = le32(buf + 0x150);
            free_hi   = le32(buf + 0x158);
        }
        sb->blocks_count      = ((uint64_t)blocks_hi << 32) | blocks_lo;
        sb->free_blocks_count = ((uint64_t)free_hi   << 32) | free_lo;
    }

    return 0;
}
