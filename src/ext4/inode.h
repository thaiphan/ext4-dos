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
    uint32_t mtime;         /* modification time (Unix seconds since 1970) */
    uint8_t  i_block[60];   /* extents header + entries, or block pointers */
};

int ext4_inode_read(struct ext4_fs *fs, uint32_t ino, struct ext4_inode *out);

/* Recompute and store the ext4 inode checksum if the FS has
 * metadata_csum enabled. raw_inode_bytes points at the inode's bytes
 * within an FS-block buffer (typically `block_buf + offset_in_block`).
 * inode_size is fs->sb.inode_size (128 = no high16, >=156 = high16 in
 * the extra section).
 *
 * Formula (matching lwext4 / Linux kernel):
 *   crc = crc32c(0xFFFFFFFF, sb.uuid, 16)
 *   crc = crc32c(crc, ino_num_LE, 4)
 *   crc = crc32c(crc, generation_LE, 4)
 *   then zero the inode's csum_lo/csum_hi bytes and crc the inode bytes
 *   restore the bytes; store low 16 in csum_lo, high 16 in csum_hi.
 *
 * No-op if metadata_csum isn't set in the FS feature_ro_compat — the
 * buffer is left unmodified. */
void ext4_inode_recompute_csum(const struct ext4_fs *fs, uint32_t ino,
                               uint8_t *raw_inode_bytes);

#endif
