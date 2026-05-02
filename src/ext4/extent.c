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
        say_simple(err, err_len, "block_size > write cap (DOS DGROUP)");
        return -1;
    }

    /* Resolve the data block via the existing extent tree. No allocation
     * — phase 2's job. */
    rc = ext4_extent_lookup(fs, inode_in->i_block, logical_block, &fs_block_data);
    if (rc) {
        say_simple(err, err_len, "logical block has no extent");
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
        say_simple(err, err_len, "read inode block failed");
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

/* Compute the bitmap checksum (low 16 in low half of return, high 16
 * in high half) over the first blocks_per_group/8 bytes of the bitmap.
 * Returns 0 if metadata_csum isn't on. Output is a return value, NOT an
 * out-pointer — out-pointers to stack locals fail in TSR interrupt
 * context (SS != DS, near-pointer writes land in DGROUP at the wrong
 * offset). Same lesson as ext4_inode_recompute_csum's static byte
 * arrays. */
static uint32_t bitmap_compute_csum(const struct ext4_fs *fs,
                                    const uint8_t *bitmap_bytes) {
    uint32_t crc;
    uint32_t span = fs->sb.blocks_per_group / 8u;
    if (!(fs->sb.feature_ro_compat & 0x400u)) return 0;
    crc = crc32c(CRC32C_INIT, fs->sb.uuid, 16);
    crc = crc32c(crc, bitmap_bytes, span);
    return crc;
}

/* DGROUP statics for block-allocation result — avoids SS!=DS out-pointer
 * bugs; find_free_block sets these directly instead of returning them. */
static uint64_t alloc_candidate;         /* physical block number */
static uint64_t alloc_bitmap_phys;       /* bitmap block this was allocated from */
static uint32_t alloc_group;             /* block group index */

/* Initialize scratch_bitmap for a BLOCK_UNINIT group g: mark reserved
 * blocks (SB backup/GDT prefix, block bitmap, inode bitmap, inode table)
 * as used; the rest are free. All data comes from the read-only in-memory
 * BGD cache (not scratch_bgd which may have pending writes). */
static void init_uninit_bitmap(const struct ext4_fs *fs, uint32_t g) {
    const uint8_t *bgd = fs->bgd_buf + (uint32_t)g * fs->bgd_size;
    uint64_t group_first, bmap_phys, ibmap_phys, itable_phys;
    uint32_t itable_size, bit, b;
    uint64_t start, end;

    memset(scratch_bitmap, 0, fs->sb.block_size);

    group_first  = (uint64_t)fs->sb.first_data_block
                 + (uint64_t)g * fs->sb.blocks_per_group;
    bmap_phys    = ((fs->bgd_size >= 64u)
                   ? ((uint64_t)le32(bgd + 0x20) << 32) : 0u) | le32(bgd + 0x00);
    ibmap_phys   = ((fs->bgd_size >= 64u)
                   ? ((uint64_t)le32(bgd + 0x24) << 32) : 0u) | le32(bgd + 0x04);
    itable_phys  = ((fs->bgd_size >= 64u)
                   ? ((uint64_t)le32(bgd + 0x28) << 32) : 0u) | le32(bgd + 0x08);
    itable_size  = (fs->sb.inodes_per_group * fs->sb.inode_size
                 + fs->sb.block_size - 1u) / fs->sb.block_size;

    /* Mark [group_first .. bmap_phys-1] as used: SB backup + GDT. */
    if (bmap_phys > group_first) {
        end = bmap_phys - group_first;
        if (end > fs->sb.blocks_per_group) end = fs->sb.blocks_per_group;
        for (b = 0; b < (uint32_t)end; b++)
            scratch_bitmap[b >> 3] |= (uint8_t)(1u << (b & 7u));
    }

    /* Mark block bitmap block. */
    if (bmap_phys >= group_first) {
        bit = (uint32_t)(bmap_phys - group_first);
        if (bit < fs->sb.blocks_per_group)
            scratch_bitmap[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
    }

    /* Mark inode bitmap block. */
    if (ibmap_phys >= group_first) {
        bit = (uint32_t)(ibmap_phys - group_first);
        if (bit < fs->sb.blocks_per_group)
            scratch_bitmap[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
    }

    /* Mark inode table blocks. */
    if (itable_phys >= group_first) {
        start = itable_phys - group_first;
        end   = start + itable_size;
        if (end > fs->sb.blocks_per_group) end = fs->sb.blocks_per_group;
        for (b = (uint32_t)start; b < (uint32_t)end; b++)
            scratch_bitmap[b >> 3] |= (uint8_t)(1u << (b & 7u));
    }
}

/* Find and allocate any free block across all groups. On success:
 *   - scratch_bgd holds the updated BGD block (modified group's entry)
 *   - scratch_bitmap holds the modified block bitmap
 *   - alloc_candidate / alloc_bitmap_phys / alloc_group are set
 * On failure, err is populated and -1 returned. */
static int find_free_block(struct ext4_fs *fs, char *err, uint32_t err_len) {
    uint32_t g;
    uint32_t scan_bit, bytes_in_bitmap;
    uint64_t bgd_fs_block;
    int      rc;

    bgd_fs_block = (fs->sb.block_size > 1024u) ? 1u : 2u;

    rc = ext4_fs_read_block(fs, bgd_fs_block, scratch_bgd);
    if (rc) { say_simple(err, err_len, "BGD block read failed"); return -1; }

    for (g = 0; g < fs->bgd_count; g++) {
        uint8_t *bgd_entry = scratch_bgd + (uint32_t)g * fs->bgd_size;
        uint16_t free_lo   = le16(bgd_entry + 0x0C);
        uint32_t free_hi   = (fs->bgd_size >= 64u) ? le16(bgd_entry + 0x2C) : 0u;
        uint8_t  bg_flags  = bgd_entry[0x12];
        uint64_t bmap_phys;
        int      found = 0;

        if (free_lo == 0u && free_hi == 0u) continue;

        bmap_phys = ((fs->bgd_size >= 64u)
                     ? ((uint64_t)le32(bgd_entry + 0x20) << 32) : 0u)
                  | (uint64_t)le32(bgd_entry + 0x00);

        if (bg_flags & 0x1u) {
            /* BLOCK_UNINIT: generate the bitmap instead of reading it. */
            init_uninit_bitmap(fs, g);
        } else {
            if (ext4_fs_read_block(fs, bmap_phys, scratch_bitmap) != 0) continue;
        }

        /* Scan for a free bit. */
        bytes_in_bitmap = fs->sb.blocks_per_group >> 3;
        if (bytes_in_bitmap > EXT4_WRITE_BUF_SIZE) bytes_in_bitmap = EXT4_WRITE_BUF_SIZE;
        for (scan_bit = 0; scan_bit < bytes_in_bitmap * 8u; scan_bit++) {
            uint32_t byte = scan_bit >> 3;
            uint8_t  mask = (uint8_t)(1u << (scan_bit & 7u));
            if (!(scratch_bitmap[byte] & mask)) {
                scratch_bitmap[byte] |= mask;
                alloc_candidate   = (uint64_t)fs->sb.first_data_block
                                  + (uint64_t)g * fs->sb.blocks_per_group
                                  + scan_bit;
                alloc_bitmap_phys = bmap_phys;
                alloc_group       = g;
                found = 1;
                break;
            }
        }
        if (!found) continue;

        /* Decrement free_blocks_count in this BGD entry. */
        if (free_lo == 0u) { free_hi -= 1u; free_lo = 0xFFFFu; }
        else                { free_lo -= 1u; }
        bgd_entry[0x0C] = (uint8_t) free_lo;
        bgd_entry[0x0D] = (uint8_t)(free_lo >> 8);
        if (fs->bgd_size >= 64u) {
            bgd_entry[0x2C] = (uint8_t) free_hi;
            bgd_entry[0x2D] = (uint8_t)(free_hi >> 8);
        }

        /* Clear BLOCK_UNINIT if we initialized the bitmap ourselves. */
        if (bg_flags & 0x1u) bgd_entry[0x12] &= ~(uint8_t)0x1u;

        /* Recompute bitmap + BGD checksums. */
        {
            uint32_t bm_csum = bitmap_compute_csum(fs, scratch_bitmap);
            bgd_entry[0x18] = (uint8_t)(bm_csum & 0xFFu);
            bgd_entry[0x19] = (uint8_t)(bm_csum >>  8);
            if (fs->bgd_size >= 64u) {
                bgd_entry[0x38] = (uint8_t)(bm_csum >> 16);
                bgd_entry[0x39] = (uint8_t)(bm_csum >> 24);
            }
        }
        {
            uint16_t bg_csum = bgd_compute_csum(fs, g, bgd_entry);
            bgd_entry[0x1E] = (uint8_t) bg_csum;
            bgd_entry[0x1F] = (uint8_t)(bg_csum >> 8);
        }
        return 0;
    }

    say_simple(err, err_len, "no free block in any group");
    return -1;
}

int ext4_file_extend_block(struct ext4_fs *fs, struct ext4_inode *inode_in,
                           uint32_t inode_num, const void *new_data,
                           uint32_t now_unix, char *err, uint32_t err_len) {
    /* Reuses the file-scope scratch_* buffers — see file header. Five
     * FS-block buffers: scratch_data, scratch_bitmap, scratch_bgd,
     * scratch_sb, scratch_inode_block. */

    /* Inode-locator math (mirrors ext4_inode_read). */
    uint32_t       group;
    uint32_t       index_in_group;
    uint64_t       inode_table_block;
    uint64_t       inode_byte_in_table;
    uint64_t       inode_fs_block;
    uint32_t       inode_offset_in_block;

    /* SB locator. */
    uint64_t       sb_fs_block;
    uint32_t       sb_offset_in_block;

    /* Existing extent + candidate. */
    uint8_t       *iblock;
    uint16_t       ext_magic, ext_entries, ext_depth;
    uint32_t       ext_logical;
    uint16_t       ext_len;
    uint64_t       ext_phys;
    uint64_t       sb_free_total;
    uint8_t       *sb_in_block;
    uint8_t       *inode_in_block;
    uint32_t       new_size_lo;
    uint64_t       new_size_total;
    uint32_t       blocks_count_lo;
    uint16_t       blocks_count_hi;
    int            rc;

    if (err && err_len) err[0] = '\0';
    if (inode_num == 0) { say_simple(err, err_len, "inode 0 invalid"); return -1; }
    if (fs->sb.block_size > EXT4_WRITE_BUF_SIZE) {
        say_simple(err, err_len, "block_size > write cap (DOS DGROUP)");
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
        say_simple(err, err_len, "extent tree depth>0 — only leaf supported");
        return -1;
    }
    if (ext_entries == 0 || ext_entries > 4) {
        say_simple(err, err_len, "extent entries 0 or >4 — defer");
        return -1;
    }
    {
        const uint8_t *e = iblock + 12 + ((uint32_t)ext_entries - 1u) * 12u;
        ext_logical = le32(e + 0);
        ext_len     = le16(e + 4);
        if (ext_len & 0x8000u) {
            say_simple(err, err_len, "uninitialized extent — not handled");
            return -1;
        }
        ext_phys    = ((uint64_t)le16(e + 6) << 32) | (uint64_t)le32(e + 8);
    }
    if ((uint64_t)inode_in->size !=
        (uint64_t)(ext_logical + ext_len) * fs->sb.block_size) {
        say_simple(err, err_len, "inode size doesn't match last extent end");
        return -1;
    }

    /* --- Try contiguous block first; fall back to any-free-block scan --- */
    {
        uint32_t target_group;
        uint64_t contig = ext_phys + (uint64_t)ext_len;  /* contiguous candidate */
        int contig_in_range;

        /* Determine which group the contiguous candidate falls in. */
        contig_in_range = (contig >= (uint64_t)fs->sb.first_data_block &&
                           contig <  (uint64_t)fs->sb.first_data_block
                                   + (uint64_t)fs->bgd_count * fs->sb.blocks_per_group);
        if (contig_in_range) {
            target_group = (uint32_t)((contig - fs->sb.first_data_block)
                                      / fs->sb.blocks_per_group);
        } else {
            target_group = fs->bgd_count; /* signal: don't try contiguous */
        }

        /* If contiguous candidate is viable, try it as a "prefer" hint by
         * temporarily checking its bit — if free, great; otherwise fall
         * through to the general scanner which tries every group. The
         * general scanner in find_free_block will pick the first free bit
         * (which may or may not be contiguous). */
        if (target_group < fs->bgd_count) {
            /* Need a new leaf slot if candidate turns out non-contiguous. */
            if (ext_entries >= 4u) {
                /* Contiguous is the ONLY option without a new leaf — check
                 * the bit directly. */
                uint64_t bgd_fs_block = (fs->sb.block_size > 1024u) ? 1u : 2u;
                uint8_t *bgd_entry;
                uint64_t bmap_phys;
                uint32_t bit = (uint32_t)((contig - fs->sb.first_data_block)
                                          % fs->sb.blocks_per_group);

                rc = ext4_fs_read_block(fs, bgd_fs_block, scratch_bgd);
                if (rc) { say_simple(err, err_len, "BGD block read failed"); return -1; }
                bgd_entry = scratch_bgd + (uint32_t)target_group * fs->bgd_size;
                bmap_phys = ((fs->bgd_size >= 64u)
                             ? ((uint64_t)le32(bgd_entry + 0x20) << 32) : 0u)
                           | (uint64_t)le32(bgd_entry + 0x00);

                if (bgd_entry[0x12] & 0x1u) {
                    /* UNINIT — contiguous bit is guaranteed free. */
                    init_uninit_bitmap(fs, target_group);
                } else {
                    rc = ext4_fs_read_block(fs, bmap_phys, scratch_bitmap);
                    if (rc) { say_simple(err, err_len, "bitmap read failed"); return -1; }
                }
                if (scratch_bitmap[bit >> 3] & (uint8_t)(1u << (bit & 7u))) {
                    say_simple(err, err_len, "leaf table full and contiguous block taken");
                    return -1;
                }
                /* Allocate it. */
                scratch_bitmap[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
                alloc_candidate   = contig;
                alloc_bitmap_phys = bmap_phys;
                alloc_group       = target_group;
                if (bgd_entry[0x12] & 0x1u) bgd_entry[0x12] &= ~(uint8_t)0x1u;
                {
                    uint16_t fl = le16(bgd_entry + 0x0C);
                    uint32_t fh = (fs->bgd_size >= 64u) ? le16(bgd_entry + 0x2C) : 0u;
                    if (fl == 0u) { fh -= 1u; fl = 0xFFFFu; } else fl -= 1u;
                    bgd_entry[0x0C] = (uint8_t)fl;
                    bgd_entry[0x0D] = (uint8_t)(fl >> 8);
                    if (fs->bgd_size >= 64u) {
                        bgd_entry[0x2C] = (uint8_t)fh;
                        bgd_entry[0x2D] = (uint8_t)(fh >> 8);
                    }
                }
                {
                    uint32_t bm_csum = bitmap_compute_csum(fs, scratch_bitmap);
                    bgd_entry[0x18] = (uint8_t)(bm_csum & 0xFFu);
                    bgd_entry[0x19] = (uint8_t)(bm_csum >>  8);
                    if (fs->bgd_size >= 64u) {
                        bgd_entry[0x38] = (uint8_t)(bm_csum >> 16);
                        bgd_entry[0x39] = (uint8_t)(bm_csum >> 24);
                    }
                }
                {
                    uint16_t bc = bgd_compute_csum(fs, target_group, bgd_entry);
                    bgd_entry[0x1E] = (uint8_t)bc;
                    bgd_entry[0x1F] = (uint8_t)(bc >> 8);
                }
            } else {
                /* Leaf table has room — use the general scanner which will
                 * try every group and pick the first free bit. */
                if (find_free_block(fs, err, err_len) != 0) return -1;
            }
        } else {
            /* Contiguous candidate is out of range — general scanner. */
            if (ext_entries >= 4u) {
                say_simple(err, err_len, "leaf table full and no room for new leaf");
                return -1;
            }
            if (find_free_block(fs, err, err_len) != 0) return -1;
        }
    }

    /* --- Read the FS SB block + decrement its free_blocks_count --- */
    sb_fs_block         = (fs->sb.block_size > 1024u) ? 0u : 1u;
    sb_offset_in_block  = (fs->sb.block_size > 1024u) ? 1024u : 0u;
    rc = ext4_fs_read_block(fs, sb_fs_block, scratch_sb);
    if (rc) { say_simple(err, err_len, "fs sb block read failed"); return -1; }
    sb_in_block = scratch_sb + sb_offset_in_block;

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
        say_simple(err, err_len, "inode block read failed");
        return -1;
    }
    inode_in_block = scratch_inode_block + inode_offset_in_block;

    /* Update the extent tree. Two cases:
     *   (a) alloc_candidate == ext_phys + ext_len → contiguous; bump last
     *       leaf's ee_len by 1 (works even when ext_entries == 4)
     *   (b) else → append a new leaf at index ext_entries; the
     *       find_free_block caller path above refused ext_entries >= 4. */
    if (alloc_candidate == ext_phys + (uint64_t)ext_len) {
        uint8_t *e = inode_in_block + 0x28 + 12u
                   + ((uint32_t)ext_entries - 1u) * 12u;
        uint16_t new_len = (uint16_t)(le16(e + 4) + 1u);
        e[4] = (uint8_t) new_len;
        e[5] = (uint8_t)(new_len >> 8);
    } else {
        uint8_t *iblk = inode_in_block + 0x28;
        uint8_t *e    = iblk + 12u + (uint32_t)ext_entries * 12u;
        uint32_t new_logical = ext_logical + (uint32_t)ext_len;
        uint16_t new_entries = (uint16_t)(ext_entries + 1u);
        iblk[2] = (uint8_t) new_entries;
        iblk[3] = (uint8_t)(new_entries >> 8);
        e[0]  = (uint8_t) new_logical;
        e[1]  = (uint8_t)(new_logical >>  8);
        e[2]  = (uint8_t)(new_logical >> 16);
        e[3]  = (uint8_t)(new_logical >> 24);
        e[4]  = 1u; e[5] = 0u;
        e[6]  = (uint8_t) (uint16_t)(alloc_candidate >> 32);
        e[7]  = (uint8_t)((uint16_t)(alloc_candidate >> 32) >> 8);
        e[8]  = (uint8_t) alloc_candidate;
        e[9]  = (uint8_t)(alloc_candidate >>  8);
        e[10] = (uint8_t)(alloc_candidate >> 16);
        e[11] = (uint8_t)(alloc_candidate >> 24);
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
    scratch_trans.fs_block[0] = alloc_candidate;
    scratch_trans.buf[0]      = scratch_data;
    scratch_trans.fs_block[1] = alloc_bitmap_phys;
    scratch_trans.buf[1]      = scratch_bitmap;
    scratch_trans.fs_block[2] = (fs->sb.block_size > 1024u) ? 1u : 2u;
    scratch_trans.buf[2]      = scratch_bgd;
    scratch_trans.fs_block[3] = sb_fs_block;
    scratch_trans.buf[3]      = scratch_sb;
    scratch_trans.fs_block[4] = inode_fs_block;
    scratch_trans.buf[4]      = scratch_inode_block;

    rc = ext4_journal_commit(fs, &scratch_trans, err, err_len);
    if (rc) return rc;

    /* Update caller's parsed inode struct so subsequent reads see the
     * new mtime, size, and extent tree without re-reading the inode. */
    inode_in->mtime = now_unix;
    inode_in->size  = new_size_total;
    if (alloc_candidate == ext_phys + (uint64_t)ext_len) {
        uint8_t *e = inode_in->i_block + 12u
                   + ((uint32_t)ext_entries - 1u) * 12u;
        uint16_t new_len = (uint16_t)(le16(e + 4) + 1u);
        e[4] = (uint8_t) new_len;
        e[5] = (uint8_t)(new_len >> 8);
    } else {
        uint8_t *iblk = inode_in->i_block;
        uint8_t *e    = iblk + 12u + (uint32_t)ext_entries * 12u;
        uint32_t new_logical = ext_logical + (uint32_t)ext_len;
        uint16_t new_entries = (uint16_t)(ext_entries + 1u);
        iblk[2] = (uint8_t) new_entries;
        iblk[3] = (uint8_t)(new_entries >> 8);
        e[0]  = (uint8_t) new_logical;
        e[1]  = (uint8_t)(new_logical >>  8);
        e[2]  = (uint8_t)(new_logical >> 16);
        e[3]  = (uint8_t)(new_logical >> 24);
        e[4]  = 1u; e[5] = 0u;
        e[6]  = (uint8_t) (uint16_t)(alloc_candidate >> 32);
        e[7]  = (uint8_t)((uint16_t)(alloc_candidate >> 32) >> 8);
        e[8]  = (uint8_t) alloc_candidate;
        e[9]  = (uint8_t)(alloc_candidate >>  8);
        e[10] = (uint8_t)(alloc_candidate >> 16);
        e[11] = (uint8_t)(alloc_candidate >> 24);
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
