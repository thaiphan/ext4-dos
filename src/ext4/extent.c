#include "extent.h"
#include "fs.h"
#include "inode.h"
#include "journal.h"
#include "../blockdev/blockdev.h"
#include "../util/endian.h"
#include <stdio.h>
#include <string.h>

static void say_simple(char *err, uint32_t err_len, const char *msg) {
    if (err && err_len) {
        size_t n = strlen(msg);
        if (n >= err_len) n = err_len - 1u;
        memcpy(err, msg, n);
        err[n] = '\0';
    }
}

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

        rc = ext4_fs_read_block(fs, next_block, node_buf);
        if (rc) return -5;

        node = node_buf;
    }
}

int ext4_file_read_block(struct ext4_fs *fs, const struct ext4_inode *inode,
                         uint32_t logical_block, void *out_buf) {
    /* Static so the &phys passed to extent_lookup resolves via DS, not SS.
     * In interrupt-handler context SS != DS — extent_lookup writes *out_phys
     * with DS-relative addressing, so we need phys to live in DGROUP. */
    static uint64_t phys;
    int             rc;

    rc = ext4_extent_lookup(fs, inode->i_block, logical_block, &phys);
    if (rc) return rc;

    return ext4_fs_read_block(fs, phys, out_buf);
}

int ext4_file_write_block(struct ext4_fs *fs, struct ext4_inode *inode_in,
                          uint32_t inode_num, uint32_t logical_block,
                          const void *new_data, uint32_t now_unix,
                          char *err, uint32_t err_len) {
    /* DGROUP statics — DOS small-model. data_buf holds a copy of the
     * caller's new bytes (so the journal commit doesn't read from a
     * potentially-volatile FAR pointer); inode_buf holds the FS block
     * containing the inode, with mtime bumped. */
    static uint8_t data_buf[EXT4_EXT_NODE_BUF];
    static uint8_t inode_buf[EXT4_EXT_NODE_BUF];
    static struct ext4_jbd_trans trans;

    /* extent_lookup writes *out_phys via DS-relative addressing, so its
     * arg must live in DGROUP, not on stack. */
    static uint64_t fs_block_data;
    uint64_t       fs_block_inode;
    uint32_t       offset_in_block;
    uint32_t       group;
    uint32_t       index_in_group;
    uint64_t       inode_table_block;
    uint64_t       inode_byte_in_table;
    const uint8_t *bgd;
    int            rc;

    if (err && err_len) err[0] = '\0';
    if (inode_num == 0) { say_simple(err, err_len, "inode 0 invalid"); return -1; }
    if (fs->sb.block_size > sizeof data_buf) {
        snprintf(err, err_len, "block_size %lu > write buf",
                 (unsigned long)fs->sb.block_size);
        return -1;
    }

    /* Resolve the data block via the existing extent tree. No allocation
     * — phase 2's job. */
    rc = ext4_extent_lookup(fs, inode_in->i_block, logical_block, &fs_block_data);
    if (rc) {
        snprintf(err, err_len, "logical block %lu has no extent (rc=%d)",
                 (unsigned long)logical_block, rc);
        return rc;
    }

    /* Locate the FS block containing the inode. Mirrors ext4_inode_read's
     * math — not extracted to a shared helper to avoid touching the
     * tested read path right now. */
    group          = (inode_num - 1u) / fs->sb.inodes_per_group;
    index_in_group = (inode_num - 1u) % fs->sb.inodes_per_group;
    if (group >= fs->bgd_count) {
        say_simple(err, err_len, "inode group out of range");
        return -1;
    }
    bgd = fs->bgd_buf + (uint32_t)group * fs->bgd_size;
    {
        uint32_t lo = le32(bgd + 0x08);
        uint32_t hi = (fs->bgd_size >= 64u) ? le32(bgd + 0x28) : 0u;
        inode_table_block = ((uint64_t)hi << 32) | lo;
    }
    inode_byte_in_table = (uint64_t)index_in_group * fs->sb.inode_size;
    fs_block_inode      = inode_table_block + inode_byte_in_table / fs->sb.block_size;
    offset_in_block     = (uint32_t)(inode_byte_in_table % fs->sb.block_size);

    /* Read the FS block containing the inode (through the journal-aware
     * helper — phase 1b already requires jsb.start == 0 so this is a
     * straight on-disk read). */
    rc = ext4_fs_read_block(fs, fs_block_inode, inode_buf);
    if (rc) {
        snprintf(err, err_len, "read inode block failed (rc=%d)", rc);
        return rc;
    }

    /* Bump mtime in the inode (offset 0x10 within the inode, LE). */
    {
        uint8_t *p = inode_buf + offset_in_block + 0x10;
        p[0] = (uint8_t) now_unix;
        p[1] = (uint8_t)(now_unix >>  8);
        p[2] = (uint8_t)(now_unix >> 16);
        p[3] = (uint8_t)(now_unix >> 24);
    }

    /* Recompute the inode-level checksum (no-op if metadata_csum is
     * off). Must come after every other byte change to the inode —
     * mtime above, plus any future writes that touch other fields. */
    ext4_inode_recompute_csum(fs, inode_num, inode_buf + offset_in_block);

    /* Copy caller's new data into DGROUP so the multi-step commit doesn't
     * hold a FAR pointer alive across BIOS calls. */
    memcpy(data_buf, new_data, fs->sb.block_size);

    trans.block_count = 2u;
    trans.fs_block[0] = fs_block_data;
    trans.buf[0]      = data_buf;
    trans.fs_block[1] = fs_block_inode;
    trans.buf[1]      = inode_buf;

    rc = ext4_journal_commit(fs, &trans, err, err_len);
    if (rc) return rc;

    /* Update the caller's parsed inode struct so subsequent operations
     * see the new mtime without a re-read. */
    inode_in->mtime = now_unix;
    return 0;
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
