#include "extent.h"
#include "fs.h"
#include "inode.h"
#include "../blockdev/blockdev.h"
#include "../util/endian.h"
#include <string.h>

int ext4_extent_lookup(struct ext4_fs *fs, const uint8_t *initial_node,
                       uint32_t logical, uint64_t *out_phys) {
    static uint8_t node_buf[EXT4_EXT_NODE_BUF];
    const uint8_t *node;
    uint16_t       i;
    uint16_t       magic;
    uint16_t       entries;
    uint16_t       depth;
    uint64_t       next_block;
    int            found;
    uint32_t       best_logical;
    uint64_t       byte;
    uint64_t       sector;
    uint32_t       sectors;
    int            rc;

    if (fs->sb.block_size > sizeof node_buf) return -10;

    node = initial_node;

    for (;;) {
        magic   = le16(node + 0);
        entries = le16(node + 2);
        depth   = le16(node + 6);

        if (magic != EXT4_EXT_MAGIC) return -1;

        if (depth == 0) {
            for (i = 0; i < entries; i++) {
                const uint8_t *ext = node + 12 + i * 12;
                uint32_t ext_logical = le32(ext + 0);
                uint16_t ext_len     = le16(ext + 4);
                uint16_t start_hi    = le16(ext + 6);
                uint32_t start_lo    = le32(ext + 8);
                /* High bit of length flags an "uninitialized" extent (zero-fill
                 * on read). For v1 we treat it the same as a regular extent —
                 * the caller can decide whether to memset. */
                uint16_t real_len    = (uint16_t)(ext_len & 0x7FFFu);

                if (logical >= ext_logical && logical < ext_logical + real_len) {
                    *out_phys = ((uint64_t)start_hi << 32) | start_lo;
                    *out_phys += (logical - ext_logical);
                    return 0;
                }
            }
            return -2; /* hole or beyond mapped extents */
        }

        /* Internal node: pick the highest index whose logical_block <= logical. */
        next_block   = 0;
        found        = 0;
        best_logical = 0;
        for (i = 0; i < entries; i++) {
            const uint8_t *idx = node + 12 + i * 12;
            uint32_t idx_logical = le32(idx + 0);
            uint32_t leaf_lo     = le32(idx + 4);
            uint16_t leaf_hi     = le16(idx + 8);

            if (idx_logical <= logical && (!found || idx_logical > best_logical)) {
                best_logical = idx_logical;
                next_block   = ((uint64_t)leaf_hi << 32) | leaf_lo;
                found        = 1;
            }
        }
        if (!found) return -3;

        byte    = next_block * (uint64_t)fs->sb.block_size;
        sector  = fs->partition_lba + byte / fs->bd->sector_size;
        sectors = fs->sb.block_size / fs->bd->sector_size;

        rc = bdev_read(fs->bd, sector, sectors, node_buf);
        if (rc) return -5;

        node = node_buf;
    }
}

int ext4_file_read_block(struct ext4_fs *fs, const struct ext4_inode *inode,
                         uint32_t logical_block, void *out_buf) {
    uint64_t phys;
    uint64_t byte;
    uint64_t sector;
    uint32_t sectors;
    int      rc;

    rc = ext4_extent_lookup(fs, inode->i_block, logical_block, &phys);
    if (rc) return rc;

    byte    = phys * (uint64_t)fs->sb.block_size;
    sector  = fs->partition_lba + byte / fs->bd->sector_size;
    sectors = fs->sb.block_size / fs->bd->sector_size;

    return bdev_read(fs->bd, sector, sectors, out_buf);
}

int ext4_file_read_head(struct ext4_fs *fs, const struct ext4_inode *inode,
                        uint32_t length, void *out_buf) {
    static uint8_t blk[EXT4_EXT_NODE_BUF];
    uint8_t       *out;
    uint32_t       bs;
    uint32_t       blocks_to_read;
    uint32_t       i;
    uint32_t       copy_len;
    int            rc;

    if (inode->size < (uint64_t)length) length = (uint32_t)inode->size;
    bs = fs->sb.block_size;
    if (bs > sizeof blk) return -1;
    blocks_to_read = (length + bs - 1u) / bs;
    out = (uint8_t *)out_buf;

    for (i = 0; i < blocks_to_read; i++) {
        rc = ext4_file_read_block(fs, inode, i, blk);
        if (rc) return rc;
        copy_len = (i == blocks_to_read - 1u) ? (length - i * bs) : bs;
        memcpy(out + i * bs, blk, copy_len);
    }
    return (int)length;
}
