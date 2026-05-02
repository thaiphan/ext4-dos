#include "extent.h"
#include "fs.h"
#include "inode.h"
#include "journal.h"
#include "../blockdev/blockdev.h"
#include "../util/endian.h"
#include "../util/crc32c.h"
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

/* DGROUP-resident scratch buffers shared across the write paths. DOS
 * small-model has a 64 KiB DGROUP cap; with phase 2's five FS-block
 * write buffers we'd overflow at 4 KiB-per-buffer, so the WRITE path
 * is capped at block_size <= 1024 (read paths still support 4 KiB).
 * Both write call sites — ext4_file_write_block (uses data + inode)
 * and ext4_file_extend_block (uses all five) — refuse to run on
 * larger-block FSes. Lifetimes don't overlap; each call returns
 * before the next runs. */
#define EXT4_WRITE_BUF_SIZE 1024u
static uint8_t scratch_data       [EXT4_WRITE_BUF_SIZE]; /* file data block */
static uint8_t scratch_inode_block[EXT4_WRITE_BUF_SIZE]; /* FS block holding inode */
static uint8_t scratch_bitmap     [EXT4_WRITE_BUF_SIZE]; /* block bitmap (extend only) */
static uint8_t scratch_bgd        [EXT4_WRITE_BUF_SIZE]; /* BGD block     (extend only) */
static uint8_t scratch_sb         [EXT4_WRITE_BUF_SIZE]; /* fs sb block   (extend only) */
static struct ext4_jbd_trans scratch_trans;

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
    if (fs->sb.block_size > EXT4_WRITE_BUF_SIZE) {
        snprintf(err, err_len, "block_size %lu > write cap %u (DOS DGROUP)",
                 (unsigned long)fs->sb.block_size, (unsigned)EXT4_WRITE_BUF_SIZE);
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
    rc = ext4_fs_read_block(fs, fs_block_inode, scratch_inode_block);
    if (rc) {
        snprintf(err, err_len, "read inode block failed (rc=%d)", rc);
        return rc;
    }

    /* Bump mtime in the inode (offset 0x10 within the inode, LE). */
    {
        uint8_t *p = scratch_inode_block + offset_in_block + 0x10;
        p[0] = (uint8_t) now_unix;
        p[1] = (uint8_t)(now_unix >>  8);
        p[2] = (uint8_t)(now_unix >> 16);
        p[3] = (uint8_t)(now_unix >> 24);
    }

    /* Recompute the inode-level checksum (no-op if metadata_csum is
     * off). Must come after every other byte change to the inode —
     * mtime above, plus any future writes that touch other fields. */
    ext4_inode_recompute_csum(fs, inode_num, scratch_inode_block + offset_in_block);

    /* Copy caller's new data into DGROUP so the multi-step commit doesn't
     * hold a FAR pointer alive across BIOS calls. */
    memcpy(scratch_data, new_data, fs->sb.block_size);

    scratch_trans.block_count = 2u;
    scratch_trans.fs_block[0] = fs_block_data;
    scratch_trans.buf[0]      = scratch_data;
    scratch_trans.fs_block[1] = fs_block_inode;
    scratch_trans.buf[1]      = scratch_inode_block;

    rc = ext4_journal_commit(fs, &scratch_trans, err, err_len);
    if (rc) return rc;

    /* Update the caller's parsed inode struct so subsequent operations
     * see the new mtime without a re-read. */
    inode_in->mtime = now_unix;
    return 0;
}

/* --- Phase 2: extend a file by one contiguous block --------------------- */

/* Compute the BGD checksum (low 16 bits of crc32c) for a 32- or 64-byte
 * BGD entry at `bgd_bytes` (within a BGD block buffer). The checksum
 * field at offset 0x1E is zeroed during compute and restored afterwards.
 * Returns 0 if metadata_csum isn't on. */
static uint16_t bgd_compute_csum(const struct ext4_fs *fs, uint32_t bgid,
                                 uint8_t *bgd_bytes) {
    static uint8_t bgid_le[4];
    uint16_t saved_csum;
    uint32_t crc;
    uint16_t desc_size;

    if (!(fs->sb.feature_ro_compat & 0x400u)) return 0;

    desc_size  = fs->bgd_size;
    saved_csum = le16(bgd_bytes + 0x1E);
    bgd_bytes[0x1E] = 0;
    bgd_bytes[0x1F] = 0;

    bgid_le[0] = (uint8_t) bgid;
    bgid_le[1] = (uint8_t)(bgid >>  8);
    bgid_le[2] = (uint8_t)(bgid >> 16);
    bgid_le[3] = (uint8_t)(bgid >> 24);

    crc = crc32c(CRC32C_INIT, fs->sb.uuid, 16);
    crc = crc32c(crc, bgid_le, 4);
    crc = crc32c(crc, bgd_bytes, desc_size);

    bgd_bytes[0x1E] = (uint8_t) saved_csum;
    bgd_bytes[0x1F] = (uint8_t)(saved_csum >> 8);

    return (uint16_t)(crc & 0xFFFFu);
}

/* Compute the bitmap checksum (lo/hi halves) over the first
 * blocks_per_group/8 bytes of the bitmap. */
static void bitmap_compute_csum(const struct ext4_fs *fs,
                                const uint8_t *bitmap_bytes,
                                uint16_t *out_lo, uint16_t *out_hi) {
    uint32_t crc;
    uint32_t span = fs->sb.blocks_per_group / 8u;
    if (!(fs->sb.feature_ro_compat & 0x400u)) {
        *out_lo = 0; *out_hi = 0;
        return;
    }
    crc      = crc32c(CRC32C_INIT, fs->sb.uuid, 16);
    crc      = crc32c(crc, bitmap_bytes, span);
    *out_lo  = (uint16_t)(crc & 0xFFFFu);
    *out_hi  = (uint16_t)(crc >> 16);
}

int ext4_file_extend_block(struct ext4_fs *fs, struct ext4_inode *inode_in,
                           uint32_t inode_num, const void *new_data,
                           uint32_t now_unix, char *err, uint32_t err_len) {
    /* Reuses the file-scope scratch_* buffers — see file header. Five
     * FS-block buffers come into play: scratch_data, scratch_bitmap,
     * scratch_bgd, scratch_sb, scratch_inode_block. */

    /* Inode-locator math (mirrors ext4_inode_read). */
    uint32_t       group;
    uint32_t       index_in_group;
    uint64_t       inode_table_block;
    uint64_t       inode_byte_in_table;
    uint64_t       inode_fs_block;
    uint32_t       inode_offset_in_block;

    /* BGD/SB locator. */
    uint64_t       bgd_fs_block;
    uint64_t       sb_fs_block;
    uint32_t       sb_offset_in_block;

    /* Existing extent + candidate. */
    uint8_t       *iblock;
    uint16_t       ext_magic, ext_entries, ext_depth;
    uint32_t       ext_logical;
    uint16_t       ext_len;
    uint64_t       ext_phys;
    uint64_t       candidate;
    uint64_t       bitmap_block_phys;
    uint32_t       bit_in_group;
    uint16_t       free_blocks_lo;
    uint32_t       free_blocks_hi32;
    uint64_t       sb_free_total;
    uint16_t       new_csum_lo, new_csum_hi;
    uint16_t       new_bg_csum;
    uint8_t       *bgd_entry;
    uint8_t       *sb_in_block;
    uint8_t       *inode_in_block;
    uint32_t       new_size_lo;
    uint64_t       new_size_total;
    uint32_t       blocks_count_lo;
    uint16_t       blocks_count_hi;
    int            rc;
    uint8_t        bg_flags_lo;

    if (err && err_len) err[0] = '\0';
    if (inode_num == 0) { say_simple(err, err_len, "inode 0 invalid"); return -1; }
    if (fs->sb.block_size > EXT4_WRITE_BUF_SIZE) {
        snprintf(err, err_len, "block_size %lu > write cap %u (DOS DGROUP)",
                 (unsigned long)fs->sb.block_size, (unsigned)EXT4_WRITE_BUF_SIZE);
        return -1;
    }

    /* --- Validate preconditions on the inode's extent tree --- */
    iblock      = inode_in->i_block;
    ext_magic   = le16(iblock + 0);
    ext_entries = le16(iblock + 2);
    ext_depth   = le16(iblock + 6);
    if (ext_magic != EXT4_EXT_MAGIC) {
        say_simple(err, err_len, "inode lacks extent header");
        return -1;
    }
    if (ext_depth != 0) {
        snprintf(err, err_len, "extent tree depth %u — phase 2 only handles leaf",
                 (unsigned)ext_depth);
        return -1;
    }
    if (ext_entries != 1) {
        snprintf(err, err_len, "%u leaf extents — phase 2 only handles single-extent",
                 (unsigned)ext_entries);
        return -1;
    }
    {
        const uint8_t *e = iblock + 12;
        ext_logical = le32(e + 0);
        ext_len     = le16(e + 4);
        if (ext_len & 0x8000u) {
            say_simple(err, err_len, "uninitialized extent — not handled");
            return -1;
        }
        ext_phys    = ((uint64_t)le16(e + 6) << 32) | (uint64_t)le32(e + 8);
    }
    if ((uint64_t)inode_in->size != (uint64_t)ext_logical * fs->sb.block_size
                                  + (uint64_t)ext_len * fs->sb.block_size) {
        /* The single extent doesn't cover all of inode.size — there'd
         * be a hole or stale state. Refuse rather than guess. */
        snprintf(err, err_len, "inode size %lu doesn't match extent coverage",
                 (unsigned long)inode_in->size);
        return -1;
    }

    candidate = ext_phys + (uint64_t)ext_len;

    /* --- Pin to group 0 for now. Iterating groups is phase 2.5. --- */
    if (candidate < fs->sb.first_data_block
        || candidate >= (uint64_t)fs->sb.first_data_block + fs->sb.blocks_per_group) {
        say_simple(err, err_len, "contiguous candidate outside group 0 — defer to phase 2.5");
        return -1;
    }
    bit_in_group = (uint32_t)(candidate - fs->sb.first_data_block);

    /* --- Read the BGD block (BGD table for our fs sits at block 1 or 2) --- */
    bgd_fs_block = (fs->sb.block_size > 1024u) ? 1u : 2u;
    rc = ext4_fs_read_block(fs, bgd_fs_block, scratch_bgd);
    if (rc) {
        snprintf(err, err_len, "BGD block read failed (rc=%d)", rc);
        return -1;
    }
    bgd_entry = scratch_bgd + 0u; /* BGD entry 0 is at offset 0 of the block */

    /* Refuse uninitialized bitmaps for now. */
    bg_flags_lo = bgd_entry[0x12]; /* bg_flags low byte */
    if (bg_flags_lo & 0x1u) {
        say_simple(err, err_len, "group 0 BLOCK_UNINIT — bitmap initialization not handled");
        return -1;
    }

    bitmap_block_phys = ((fs->bgd_size >= 64u)
                         ? ((uint64_t)le32(bgd_entry + 0x20) << 32)
                         : 0u)
                      | (uint64_t)le32(bgd_entry + 0x00);

    free_blocks_lo  = le16(bgd_entry + 0x0C);
    if (fs->bgd_size >= 64u) {
        free_blocks_hi32 = le16(bgd_entry + 0x2C);
    } else {
        free_blocks_hi32 = 0;
    }

    /* --- Read the bitmap block --- */
    rc = ext4_fs_read_block(fs, bitmap_block_phys, scratch_bitmap);
    if (rc) {
        snprintf(err, err_len, "bitmap block read failed (rc=%d)", rc);
        return -1;
    }

    /* Check the candidate bit (LE bit order: byte = bit/8, bit = bit%8). */
    {
        uint32_t byte = bit_in_group >> 3;
        uint8_t  mask = (uint8_t)(1u << (bit_in_group & 7u));
        if (scratch_bitmap[byte] & mask) {
            snprintf(err, err_len, "candidate block %lu already allocated",
                     (unsigned long)candidate);
            return -1;
        }
        scratch_bitmap[byte] |= mask;
    }

    /* --- Read the FS SB block --- */
    sb_fs_block         = (fs->sb.block_size > 1024u) ? 0u : 1u;
    sb_offset_in_block  = (fs->sb.block_size > 1024u) ? 1024u : 0u;
    rc = ext4_fs_read_block(fs, sb_fs_block, scratch_sb);
    if (rc) {
        snprintf(err, err_len, "fs sb block read failed (rc=%d)", rc);
        return -1;
    }
    sb_in_block = scratch_sb + sb_offset_in_block;

    /* --- Decrement free_blocks_count in BGD entry --- */
    if (free_blocks_lo == 0u && free_blocks_hi32 == 0u) {
        say_simple(err, err_len, "BGD reports no free blocks");
        return -1;
    }
    if (free_blocks_lo == 0u) {
        free_blocks_hi32 -= 1u;
        free_blocks_lo    = 0xFFFFu;
    } else {
        free_blocks_lo -= 1u;
    }
    bgd_entry[0x0C] = (uint8_t) free_blocks_lo;
    bgd_entry[0x0D] = (uint8_t)(free_blocks_lo >> 8);
    if (fs->bgd_size >= 64u) {
        bgd_entry[0x2C] = (uint8_t) free_blocks_hi32;
        bgd_entry[0x2D] = (uint8_t)(free_blocks_hi32 >> 8);
    }

    /* Recompute bitmap csum + write into BGD entry; recompute BGD csum. */
    bitmap_compute_csum(fs, scratch_bitmap, &new_csum_lo, &new_csum_hi);
    bgd_entry[0x18] = (uint8_t) new_csum_lo;
    bgd_entry[0x19] = (uint8_t)(new_csum_lo >> 8);
    if (fs->bgd_size >= 64u) {
        bgd_entry[0x38] = (uint8_t) new_csum_hi;
        bgd_entry[0x39] = (uint8_t)(new_csum_hi >> 8);
    }
    new_bg_csum = bgd_compute_csum(fs, /*bgid=*/0u, bgd_entry);
    bgd_entry[0x1E] = (uint8_t) new_bg_csum;
    bgd_entry[0x1F] = (uint8_t)(new_bg_csum >> 8);

    /* --- Decrement fs sb free_blocks_count (LE 64-bit) --- */
    {
        uint32_t lo = le32(sb_in_block + 0x0C);
        uint32_t hi = (fs->sb.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
                      ? le32(sb_in_block + 0x158) : 0u;
        sb_free_total = ((uint64_t)hi << 32) | lo;
        if (sb_free_total == 0u) {
            say_simple(err, err_len, "fs sb reports no free blocks");
            return -1;
        }
        sb_free_total -= 1u;
        sb_in_block[0x0C] = (uint8_t) sb_free_total;
        sb_in_block[0x0D] = (uint8_t)(sb_free_total >>  8);
        sb_in_block[0x0E] = (uint8_t)(sb_free_total >> 16);
        sb_in_block[0x0F] = (uint8_t)(sb_free_total >> 24);
        if (fs->sb.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
            uint32_t hi32 = (uint32_t)(sb_free_total >> 32);
            sb_in_block[0x158] = (uint8_t) hi32;
            sb_in_block[0x159] = (uint8_t)(hi32 >>  8);
            sb_in_block[0x15A] = (uint8_t)(hi32 >> 16);
            sb_in_block[0x15B] = (uint8_t)(hi32 >> 24);
        }
    }

    /* Recompute fs sb checksum if metadata_csum on. */
    if (fs->sb.feature_ro_compat & 0x400u) {
        uint32_t csum;
        sb_in_block[0x3FC] = sb_in_block[0x3FD] = 0;
        sb_in_block[0x3FE] = sb_in_block[0x3FF] = 0;
        csum = crc32c(CRC32C_INIT, sb_in_block, 0x3FC);
        sb_in_block[0x3FC] = (uint8_t) csum;
        sb_in_block[0x3FD] = (uint8_t)(csum >>  8);
        sb_in_block[0x3FE] = (uint8_t)(csum >> 16);
        sb_in_block[0x3FF] = (uint8_t)(csum >> 24);
    }

    /* --- Read the inode block + update extent length, size, blocks_count, mtime --- */
    group               = (inode_num - 1u) / fs->sb.inodes_per_group;
    index_in_group      = (inode_num - 1u) % fs->sb.inodes_per_group;
    if (group >= fs->bgd_count) {
        say_simple(err, err_len, "inode group out of range");
        return -1;
    }
    {
        const uint8_t *bgd0 = fs->bgd_buf + (uint32_t)group * fs->bgd_size;
        uint32_t lo = le32(bgd0 + 0x08);
        uint32_t hi = (fs->bgd_size >= 64u) ? le32(bgd0 + 0x28) : 0u;
        inode_table_block = ((uint64_t)hi << 32) | lo;
    }
    inode_byte_in_table   = (uint64_t)index_in_group * fs->sb.inode_size;
    inode_fs_block        = inode_table_block + inode_byte_in_table / fs->sb.block_size;
    inode_offset_in_block = (uint32_t)(inode_byte_in_table % fs->sb.block_size);

    rc = ext4_fs_read_block(fs, inode_fs_block, scratch_inode_block);
    if (rc) {
        snprintf(err, err_len, "inode block read failed (rc=%d)", rc);
        return -1;
    }
    inode_in_block = scratch_inode_block + inode_offset_in_block;

    /* Bump the leaf extent's ee_len by 1 (preserving the high bit which
     * marks initialized vs uninitialized — we already refused the
     * uninitialized case so the high bit is 0). */
    {
        uint8_t *e = inode_in_block + 0x28 + 12; /* iblock + 12 = first leaf */
        uint16_t new_len = (uint16_t)(le16(e + 4) + 1u);
        e[4] = (uint8_t) new_len;
        e[5] = (uint8_t)(new_len >> 8);
    }

    /* Bump i_size_lo and i_size_hi by block_size. Phase 2 only touches
     * sub-4GB files, so size_hi typically stays 0. */
    new_size_total = (uint64_t)inode_in->size + fs->sb.block_size;
    new_size_lo    = (uint32_t)(new_size_total & 0xFFFFFFFFul);
    inode_in_block[0x04] = (uint8_t) new_size_lo;
    inode_in_block[0x05] = (uint8_t)(new_size_lo >>  8);
    inode_in_block[0x06] = (uint8_t)(new_size_lo >> 16);
    inode_in_block[0x07] = (uint8_t)(new_size_lo >> 24);
    {
        uint32_t hi = (uint32_t)(new_size_total >> 32);
        inode_in_block[0x6C] = (uint8_t) hi;
        inode_in_block[0x6D] = (uint8_t)(hi >>  8);
        inode_in_block[0x6E] = (uint8_t)(hi >> 16);
        inode_in_block[0x6F] = (uint8_t)(hi >> 24);
    }

    /* Bump i_blocks_count_lo by (block_size / 512) — units are 512-byte
     * sectors. blocks_count_hi is in the extra section; for sub-4GB
     * files it stays 0. */
    blocks_count_lo = le32(inode_in_block + 0x1C);
    blocks_count_lo += fs->sb.block_size / 512u;
    inode_in_block[0x1C] = (uint8_t) blocks_count_lo;
    inode_in_block[0x1D] = (uint8_t)(blocks_count_lo >>  8);
    inode_in_block[0x1E] = (uint8_t)(blocks_count_lo >> 16);
    inode_in_block[0x1F] = (uint8_t)(blocks_count_lo >> 24);
    (void)blocks_count_hi;

    /* mtime + recompute i_checksum (must be last in-inode change). */
    inode_in_block[0x10] = (uint8_t) now_unix;
    inode_in_block[0x11] = (uint8_t)(now_unix >>  8);
    inode_in_block[0x12] = (uint8_t)(now_unix >> 16);
    inode_in_block[0x13] = (uint8_t)(now_unix >> 24);
    ext4_inode_recompute_csum(fs, inode_num, inode_in_block);

    /* --- Stage data block + commit 5-block transaction --- */
    memcpy(scratch_data, new_data, fs->sb.block_size);

    scratch_trans.block_count = 5u;
    scratch_trans.fs_block[0] = candidate;       /* the new data block */
    scratch_trans.buf[0]      = scratch_data;
    scratch_trans.fs_block[1] = bitmap_block_phys;
    scratch_trans.buf[1]      = scratch_bitmap;
    scratch_trans.fs_block[2] = bgd_fs_block;
    scratch_trans.buf[2]      = scratch_bgd;
    scratch_trans.fs_block[3] = sb_fs_block;
    scratch_trans.buf[3]      = scratch_sb;
    scratch_trans.fs_block[4] = inode_fs_block;
    scratch_trans.buf[4]      = scratch_inode_block;

    rc = ext4_journal_commit(fs, &scratch_trans, err, err_len);
    if (rc) return rc;

    /* Update caller's parsed inode. */
    inode_in->mtime = now_unix;
    inode_in->size  = new_size_total;
    {
        uint8_t *e = inode_in->i_block + 12;
        uint16_t new_len = (uint16_t)(le16(e + 4) + 1u);
        e[4] = (uint8_t) new_len;
        e[5] = (uint8_t)(new_len >> 8);
    }
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
