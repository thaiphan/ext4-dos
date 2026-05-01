#ifndef EXT4_INODE_H
#define EXT4_INODE_H

#include <stdint.h>

#define EXT4_INODE_FLAG_EXTENTS 0x00080000u

#define EXT4_S_IFMT  0xF000u
#define EXT4_S_IFREG 0x8000u
#define EXT4_S_IFDIR 0x4000u
#define EXT4_S_IFLNK 0xA000u

struct ext4_fs;

struct ext4_inode {
    uint16_t mode;
    uint64_t size;          /* size_lo combined with size_hi */
    uint32_t flags;
    uint8_t  i_block[60];   /* extents header + entries, or block pointers */
};

int ext4_inode_read(struct ext4_fs *fs, uint32_t ino, struct ext4_inode *out);

#endif
