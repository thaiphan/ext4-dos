#ifndef EXT4_SUPERBLOCK_H
#define EXT4_SUPERBLOCK_H

#include <stdint.h>

#define EXT4_SUPERBLOCK_OFFSET 1024
#define EXT4_SUPERBLOCK_SIZE   1024
#define EXT4_SUPERBLOCK_MAGIC  0xEF53u

#define EXT4_FEATURE_INCOMPAT_64BIT 0x00000080u

struct ext4_superblock {
    uint32_t inodes_count;
    uint64_t blocks_count;
    uint64_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint16_t magic;
    uint16_t state;
    uint32_t rev_level;
    uint16_t inode_size;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t  uuid[16];
    char     volume_name[17];
    char     last_mounted[65];
    uint8_t  journal_uuid[16];   /* matches journal-internal UUID; used as CRC32C seed */
    uint32_t journal_inum;       /* inode # of the journal file (typically 8) */
};

int ext4_superblock_parse(const uint8_t *buf, struct ext4_superblock *sb);

#endif
