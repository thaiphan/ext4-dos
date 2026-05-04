#include "inode.h"
#include "fs.h"
#include "../blockdev/blockdev.h"
#include "../util/endian.h"
#include "../util/crc32c.h"
#include <string.h>

int ext4_inode_read(struct ext4_fs *fs, uint32_t ino, struct ext4_inode *out) {
    uint32_t       group;
    uint32_t       index_in_group;
    const uint8_t *bgd;
    uint32_t       inode_table_lo;
    uint32_t       inode_table_hi;
    uint64_t       inode_table_block;
    uint64_t       inode_byte_in_table;
    uint64_t       fs_block;
    uint32_t       offset_in_block;
    /* Static so this lives in DGROUP — the bdev_read path stores
     * its segment as DS, which is wrong for stack locals when called
     * from an interrupt context (SS != DS). Sized to hold a full FS
     * block since journaling tracks at FS-block granularity. */
    static uint8_t buf[4096];
    const uint8_t *raw;
    uint32_t       size_lo;
    uint32_t       size_hi;
    int            rc;

    if (ino == 0) return -1;

    group          = (ino - 1u) / fs->sb.inodes_per_group;
    index_in_group = (ino - 1u) % fs->sb.inodes_per_group;
    bgd = ext4_fs_bgd_get(fs, group);
    if (bgd == NULL) return -2;
    inode_table_lo = le32(bgd + 0x08);
    inode_table_hi = (fs->bgd_size >= 64u) ? le32(bgd + 0x28) : 0u;
    inode_table_block = ((uint64_t)inode_table_hi << 32) | inode_table_lo;

    if (fs->sb.inode_size > 1024u) return -3;
    if (fs->sb.block_size > sizeof buf) return -4;

    /* Read the whole FS block that contains this inode so the journal
     * redirect can substitute it block-for-block if needed. */
    inode_byte_in_table = (uint64_t)index_in_group * fs->sb.inode_size;
    fs_block            = inode_table_block + inode_byte_in_table / fs->sb.block_size;
    offset_in_block     = (uint32_t)(inode_byte_in_table % fs->sb.block_size);

    rc = ext4_fs_read_block(fs, fs_block, buf);
    if (rc) return -5;

    raw = buf + offset_in_block;
    out->mode  = le16(raw + 0x00);
    out->flags = le32(raw + 0x20);
    out->mtime = le32(raw + 0x10);
    memcpy(out->i_block, raw + 0x28, 60);

    size_lo = le32(raw + 0x04);
    size_hi = le32(raw + 0x6C);
    out->size = ((uint64_t)size_hi << 32) | size_lo;

    return 0;
}

void ext4_inode_recompute_csum(const struct ext4_fs *fs, uint32_t ino,
                               uint8_t *raw_inode_bytes) {
    /* Inode-level checksum bookkeeping (FS metadata_csum, separate from
     * jbd2's per-tag csum). Layout:
     *   - 16-byte UUID seed: crc32c(0xFFFFFFFF, sb.uuid, 16)
     *   - then crc32c the LE inode number, then the LE i_generation
     *     (offset 0x64 in the inode), then the inode bytes with the two
     *     csum fields (lo at 0x7C, hi at 0x82) zeroed.
     * Store low 16 bits at 0x7C; for inode_size > 128, store high 16
     * bits at 0x82. */
    uint8_t  saved_lo[2];
    uint8_t  saved_hi[2];
    uint32_t crc;
    uint32_t generation;
    uint32_t inum_le;
    uint32_t gen_le;
    uint16_t inode_size;
    uint8_t  has_hi;

    if (!(fs->sb.feature_ro_compat & 0x400u)) return; /* metadata_csum off */

    inode_size = fs->sb.inode_size;
    has_hi     = (inode_size > 128u) ? 1u : 0u;

    saved_lo[0] = raw_inode_bytes[0x7C];
    saved_lo[1] = raw_inode_bytes[0x7D];
    raw_inode_bytes[0x7C] = 0;
    raw_inode_bytes[0x7D] = 0;
    if (has_hi) {
        saved_hi[0] = raw_inode_bytes[0x82];
        saved_hi[1] = raw_inode_bytes[0x83];
        raw_inode_bytes[0x82] = 0;
        raw_inode_bytes[0x83] = 0;
    }

    /* Inode number & generation are mixed in LE — the on-disk format.
     * These scratch arrays MUST be static — DOS small-model crc32c
     * reads its buf pointer DS-relative, but in TSR interrupt context
     * SS != DS, so a stack-local would yield random DGROUP bytes
     * instead. (host_write_test against the metadata_csum fixture
     * passes on the host because SS == DS there; the bug only surfaces
     * post-DOS-write where e2fsck flags the inode checksum.) */
    {
        static uint8_t inum_bytes[4];
        static uint8_t gen_bytes[4];

        inum_le = ino;
        inum_bytes[0] = (uint8_t) inum_le;
        inum_bytes[1] = (uint8_t)(inum_le >>  8);
        inum_bytes[2] = (uint8_t)(inum_le >> 16);
        inum_bytes[3] = (uint8_t)(inum_le >> 24);
        crc = crc32c(CRC32C_INIT, fs->sb.uuid, 16);
        crc = crc32c(crc, inum_bytes, 4);

        generation = le32(raw_inode_bytes + 0x64);
        gen_le = generation;
        gen_bytes[0] = (uint8_t) gen_le;
        gen_bytes[1] = (uint8_t)(gen_le >>  8);
        gen_bytes[2] = (uint8_t)(gen_le >> 16);
        gen_bytes[3] = (uint8_t)(gen_le >> 24);
        crc = crc32c(crc, gen_bytes, 4);
    }
    crc = crc32c(crc, raw_inode_bytes, inode_size);

    /* Restore the bytes we zeroed (the saved values, not the new csum
     * — we'll rewrite them below). */
    raw_inode_bytes[0x7C] = saved_lo[0];
    raw_inode_bytes[0x7D] = saved_lo[1];
    if (has_hi) {
        raw_inode_bytes[0x82] = saved_hi[0];
        raw_inode_bytes[0x83] = saved_hi[1];
    }

    /* Now write the new csum (LE). */
    raw_inode_bytes[0x7C] = (uint8_t) crc;
    raw_inode_bytes[0x7D] = (uint8_t)(crc >> 8);
    if (has_hi) {
        raw_inode_bytes[0x82] = (uint8_t)(crc >> 16);
        raw_inode_bytes[0x83] = (uint8_t)(crc >> 24);
    }
}
