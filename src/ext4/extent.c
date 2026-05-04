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
 * small-model has a 64 KiB DGROUP cap; with the write path's five
 * FS-block buffers we'd overflow at 4 KiB-per-buffer, so the WRITE path
 * is capped at block_size <= 1024 (read paths still support 4 KiB).
 * Both write call sites — ext4_file_write_block (uses data + inode)
 * and ext4_file_extend_block (uses all five) — refuse to run on
 * larger-block FSes. Lifetimes don't overlap; each call returns
 * before the next runs. */
#define EXT4_WRITE_BUF_SIZE 1024u
static uint8_t scratch_data         [EXT4_WRITE_BUF_SIZE]; /* file/new-dir data block */
static uint8_t scratch_inode_block  [EXT4_WRITE_BUF_SIZE]; /* FS block holding inode */
static uint8_t scratch_bitmap       [EXT4_WRITE_BUF_SIZE]; /* block/inode bitmap */
static uint8_t scratch_bgd          [EXT4_WRITE_BUF_SIZE]; /* BGD block */
static uint8_t scratch_sb           [EXT4_WRITE_BUF_SIZE]; /* fs sb block */
static uint8_t scratch_parent_dir   [EXT4_WRITE_BUF_SIZE]; /* parent dir block (mkdir) */
static uint8_t scratch_parent_inode [EXT4_WRITE_BUF_SIZE]; /* parent inode block (mkdir) */
static struct ext4_jbd_trans scratch_trans;

/* Forward decls for static helpers used before their definition. */
static void inode_set_mtime_and_csum(const struct ext4_fs *fs, uint32_t ino,
                                     uint8_t *inode_bytes, uint32_t now_unix);
static void sb_recompute_csum(const struct ext4_fs *fs, uint8_t *sb);
static int  dir_find_entry_by_inode(const uint8_t *blk, uint32_t bs,
                                    uint32_t target_ino);
static int  dir_find_slot_for_entry(uint8_t *blk, uint32_t bs,
                                    uint32_t need_rec);
static void bgd_finalize_block_bitmap_csums(const struct ext4_fs *fs,
                                            uint32_t group, uint8_t *be,
                                            const uint8_t *bitmap);
static void bgd_finalize_inode_bitmap_csums(const struct ext4_fs *fs,
                                            uint32_t group, uint8_t *be,
                                            const uint8_t *bitmap);

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
     * — not yet allocated means the call fails here. */
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
     * helper — requires jsb.start == 0 so this is a straight on-disk
     * read). */
    rc = ext4_fs_read_block(fs, fs_block_inode, scratch_inode_block);
    if (rc) {
        say_simple(err, err_len, "read inode block failed");
        return rc;
    }

    /* Bump mtime in the inode (offset 0x10 within the inode, LE). */
    /* mtime + i_checksum (must be last in-inode write). */
    inode_set_mtime_and_csum(fs, inode_num,
                             scratch_inode_block + offset_in_block, now_unix);

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

/* --- Block extent append ------------------------------------------------- */

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

/* --- File creation ------------------------------------------------------- */

/* Inode allocation result — parallel to alloc_candidate / alloc_group. */
static uint32_t alloc_inode_num;   /* 1-based inode number */
static uint32_t alloc_inode_group; /* block group */

/* Compute inode bitmap checksum. Same formula as block bitmap —
 * crc32c(uuid + bitmap_bytes) — NO bgid included. (Verified against
 * lwext4's ext4_ialloc_bitmap_csum which also omits bgid.) */
static uint32_t inode_bitmap_csum(const struct ext4_fs *fs,
                                  const uint8_t *bitmap_bytes) {
    uint32_t crc;
    uint32_t span;
    if (!(fs->sb.feature_ro_compat & 0x400u)) return 0;
    span = fs->sb.inodes_per_group >> 3;
    crc = crc32c(CRC32C_INIT, fs->sb.uuid, 16);
    crc = crc32c(crc, bitmap_bytes, span);
    return crc;
}

/* Compute directory-block tail checksum:
 * crc32c(uuid + parent_ino_LE + parent_generation_LE + dir_block[0..bs-12]). */
static uint32_t dir_block_csum(const struct ext4_fs *fs, uint32_t parent_ino,
                                uint32_t parent_gen, const uint8_t *dir_blk) {
    static uint8_t ino_le[4], gen_le[4];
    uint32_t crc;
    if (!(fs->sb.feature_ro_compat & 0x400u)) return 0;
    ino_le[0] = (uint8_t)parent_ino;       ino_le[1] = (uint8_t)(parent_ino>>8);
    ino_le[2] = (uint8_t)(parent_ino>>16); ino_le[3] = (uint8_t)(parent_ino>>24);
    gen_le[0] = (uint8_t)parent_gen;       gen_le[1] = (uint8_t)(parent_gen>>8);
    gen_le[2] = (uint8_t)(parent_gen>>16); gen_le[3] = (uint8_t)(parent_gen>>24);
    crc = crc32c(CRC32C_INIT, fs->sb.uuid, 16);
    crc = crc32c(crc, ino_le, 4);
    crc = crc32c(crc, gen_le, 4);
    crc = crc32c(crc, dir_blk, fs->sb.block_size - 12u);
    return crc;
}

/* DGROUP-resident output cells for the dir-walk helpers.  Returning via
 * out-pointers to caller-stack locals would break in TSR interrupt
 * context (SS != DS, the near pointer hits the wrong segment).  These
 * statics live in DGROUP, so writes from the helper land in the same
 * segment the caller reads from. */
static uint32_t s_dir_slot_off;
/* rec_len the caller should give the new entry — equals the predecessor's
 * pre-truncation rec_len minus the predecessor's real_sz. Filling the
 * predecessor's dead space exactly preserves any entries after it.
 * Computing as `block_size - slot_off` is the wrong formula when the slot
 * is interior (predecessor isn't the last entry); using that would
 * overwrite all entries that follow. */
static uint32_t s_dir_slot_rec;
static uint32_t s_dir_entry_off;
static uint32_t s_dir_prev_off;

/* Find a slot in `blk` (a directory block, `bs` bytes) with at least
 * `need_rec` bytes of free space after the existing entry's real size.
 * On success: returns 1, writes s_dir_slot_off (where the new entry
 * goes, = `predecessor_off + predecessor_real_sz`), and TRUNCATES the
 * predecessor's rec_len to its real size in `blk` so the caller can
 * just write the new entry at s_dir_slot_off and set its rec_len to
 * fill the rest of the slot.  Returns 0 if no slot fits. */
static int dir_find_slot_for_entry(uint8_t *blk, uint32_t bs,
                                   uint32_t need_rec) {
    uint32_t off = 0;
    while (off + 8u <= bs) {
        uint16_t rec = le16(blk + off + 4);
        uint8_t  nl  = blk[off + 6];
        uint32_t ino = le32(blk + off);
        uint32_t real_sz;
        if (rec < 8u || rec > bs - off) return 0;
        if (blk[off+7] == 0xDEu && rec == 12u && ino == 0u) {
            off += rec; continue;
        }
        real_sz = (ino != 0u) ? ((8u + nl + 3u) & ~(uint32_t)3u) : 0u;
        if (rec >= real_sz + need_rec) {
            s_dir_slot_off = off + real_sz;
            s_dir_slot_rec = (uint32_t)rec - real_sz;
            if (ino != 0u) {
                blk[off + 4] = (uint8_t) real_sz;
                blk[off + 5] = (uint8_t)(real_sz >> 8);
            }
            return 1;
        }
        off += rec;
    }
    return 0;
}

/* Walk a directory block looking for the entry with the given inode.
 * Returns 1 on found and writes s_dir_entry_off (start of entry) +
 * s_dir_prev_off (start of preceding entry, or 0xFFFFFFFFu if
 * `target_ino` is the first non-sentinel entry). Returns 0 if the
 * entry is missing. `bs` is the dir block size. */
static int dir_find_entry_by_inode(const uint8_t *blk, uint32_t bs,
                                   uint32_t target_ino) {
    uint32_t off = 0, prev = 0xFFFFFFFFu;
    while (off + 8u <= bs) {
        uint16_t rec = le16(blk + off + 4);
        uint32_t ino = le32(blk + off);
        if (rec < 8u || rec > bs - off) return 0;
        if (blk[off+7] == 0xDEu && rec == 12u && ino == 0u) {
            off += rec; continue;
        }
        if (ino == target_ino) {
            s_dir_entry_off = off; s_dir_prev_off = prev; return 1;
        }
        prev = off; off += rec;
    }
    return 0;
}

/* Stamp the block-bitmap checksum into a BGD entry's block-bitmap-csum
 * fields (low at 0x18, high at 0x38 if bgd_size >= 64), then recompute
 * the BGD entry's own checksum.  Call AFTER all other BGD-entry field
 * writes; the entry csum is over all earlier fields. */
static void bgd_finalize_block_bitmap_csums(const struct ext4_fs *fs,
                                            uint32_t group, uint8_t *be,
                                            const uint8_t *bitmap) {
    uint32_t bcs = bitmap_compute_csum(fs, bitmap);
    uint16_t bc;
    be[0x18] = (uint8_t)bcs; be[0x19] = (uint8_t)(bcs >> 8);
    if (fs->bgd_size >= 64u) {
        be[0x38] = (uint8_t)(bcs >> 16); be[0x39] = (uint8_t)(bcs >> 24);
    }
    bc = bgd_compute_csum(fs, group, be);
    be[0x1E] = (uint8_t)bc; be[0x1F] = (uint8_t)(bc >> 8);
}

/* Same but for inode-bitmap-csum fields (low at 0x1A, high at 0x3A). */
static void bgd_finalize_inode_bitmap_csums(const struct ext4_fs *fs,
                                            uint32_t group, uint8_t *be,
                                            const uint8_t *bitmap) {
    uint32_t ics = inode_bitmap_csum(fs, bitmap);
    uint16_t bc;
    be[0x1A] = (uint8_t)ics; be[0x1B] = (uint8_t)(ics >> 8);
    if (fs->bgd_size >= 64u) {
        be[0x3A] = (uint8_t)(ics >> 16); be[0x3B] = (uint8_t)(ics >> 24);
    }
    bc = bgd_compute_csum(fs, group, be);
    be[0x1E] = (uint8_t)bc; be[0x1F] = (uint8_t)(bc >> 8);
}

/* Recompute the SB checksum after modifying any SB fields.  No-op if
 * metadata_csum isn't enabled.  `sb` points at the SB at its in-block
 * offset (typically `scratch_sb + sb_offset_in_block`). */
static void sb_recompute_csum(const struct ext4_fs *fs, uint8_t *sb) {
    uint32_t cs;
    if (!(fs->sb.feature_ro_compat & 0x400u)) return;
    sb[0x3FC] = sb[0x3FD] = 0; sb[0x3FE] = sb[0x3FF] = 0;
    cs = crc32c(CRC32C_INIT, sb, 0x3FC);
    sb[0x3FC] = (uint8_t) cs; sb[0x3FD] = (uint8_t)(cs >>  8);
    sb[0x3FE] = (uint8_t)(cs >> 16); sb[0x3FF] = (uint8_t)(cs >> 24);
}

/* Stamp `now_unix` into i_mtime and recompute the inode-level csum.
 * Caller passes the pointer to the inode's bytes WITHIN the FS-block
 * buffer (typically `block + offset_in_block`).  Use this only when
 * mtime is the LAST inode-field change; for callers that also bump
 * link count (mkdir/rmdir) or other fields, write those first then
 * call this last. */
static void inode_set_mtime_and_csum(const struct ext4_fs *fs, uint32_t ino,
                                     uint8_t *inode_bytes, uint32_t now_unix) {
    inode_bytes[0x10] = (uint8_t) now_unix;
    inode_bytes[0x11] = (uint8_t)(now_unix >>  8);
    inode_bytes[0x12] = (uint8_t)(now_unix >> 16);
    inode_bytes[0x13] = (uint8_t)(now_unix >> 24);
    ext4_inode_recompute_csum(fs, ino, inode_bytes);
}

/* DGROUP-resident outputs from read_inode_block — see s_dir_* above for
 * the same SS!=DS rationale. Caller reads these after a successful
 * call. */
static uint64_t s_inode_fs_block;
static uint32_t s_inode_offset;
static uint32_t s_inode_gen;

/* Compute the location of inode `ino`'s on-disk slot, read the FS block
 * holding it into `buf`, and (always) populate s_inode_fs_block,
 * s_inode_offset, and s_inode_gen.  Caller subsequently modifies bytes
 * at `buf + s_inode_offset` and includes `s_inode_fs_block` in the
 * transaction. */
static int read_inode_block(struct ext4_fs *fs, uint32_t ino, uint8_t *buf) {
    uint32_t g  = (ino - 1u) / fs->sb.inodes_per_group;
    uint32_t i  = (ino - 1u) % fs->sb.inodes_per_group;
    const uint8_t *b0 = fs->bgd_buf + g * fs->bgd_size;
    uint64_t it = ((uint64_t)((fs->bgd_size >= 64u) ? le32(b0 + 0x28) : 0u) << 32)
                | (uint64_t)le32(b0 + 0x08);
    uint64_t ib = (uint64_t)i * fs->sb.inode_size;
    s_inode_fs_block = it + ib / fs->sb.block_size;
    s_inode_offset   = (uint32_t)(ib % fs->sb.block_size);
    if (ext4_fs_read_block(fs, s_inode_fs_block, buf) != 0) return -1;
    s_inode_gen = le32(buf + s_inode_offset + 0x64);
    return 0;
}

/* Stamp the 12-byte tail-csum sentinel on a dir block.  No-op if
 * metadata_csum isn't on.  Used after every modification to a directory
 * block — collapsing what was an 11-line repeated boilerplate at every
 * call site.  Call ONCE after all modifications to dir_blk; the sentinel
 * fields are zeroed and the csum is computed over the whole block
 * (including the zeroed-out csum bytes). */
static void dir_block_set_tail_csum(const struct ext4_fs *fs, uint8_t *dir_blk,
                                    uint32_t parent_ino, uint32_t parent_gen) {
    uint32_t tail;
    uint32_t cs;
    if (!(fs->sb.feature_ro_compat & 0x400u)) return;
    tail = fs->sb.block_size - 12u;
    dir_blk[tail+0]=dir_blk[tail+1]=0;
    dir_blk[tail+2]=dir_blk[tail+3]=0;
    dir_blk[tail+4]=12u; dir_blk[tail+5]=0u;
    dir_blk[tail+6]=0u;  dir_blk[tail+7]=0xDEu;
    dir_blk[tail+8]=dir_blk[tail+9]=0;
    dir_blk[tail+10]=dir_blk[tail+11]=0;
    cs = dir_block_csum(fs, parent_ino, parent_gen, dir_blk);
    dir_blk[tail+8]=(uint8_t)cs;        dir_blk[tail+9]=(uint8_t)(cs>>8);
    dir_blk[tail+10]=(uint8_t)(cs>>16); dir_blk[tail+11]=(uint8_t)(cs>>24);
}

/* Find and allocate a free inode across all groups. Sets alloc_inode_num
 * and alloc_inode_group; modifies scratch_bitmap (inode bitmap) and
 * scratch_bgd (BGD block).
 *
 * Limitation: indexes BGD entries as scratch_bgd + g*bgd_size, but
 * scratch_bgd holds only ONE FS block. Filesystems whose BGD table spans
 * multiple FS blocks would index off the end. Test fixtures keep
 * bgd_count small enough that all BGDs fit in a single block. The same
 * single-block assumption is made in find_free_block. Lift both
 * together when widening to multi-block BGD tables. */
static int find_free_inode(struct ext4_fs *fs, char *err, uint32_t err_len) {
    uint32_t g, bit, bytes_in_imap;
    uint64_t bgd_fs_block;
    int      rc;

    bgd_fs_block = (fs->sb.block_size > 1024u) ? 1u : 2u;
    rc = ext4_fs_read_block(fs, bgd_fs_block, scratch_bgd);
    if (rc) { say_simple(err, err_len, "BGD block read failed (inode)"); return -1; }

    bytes_in_imap = fs->sb.inodes_per_group >> 3;
    if (bytes_in_imap > EXT4_WRITE_BUF_SIZE) bytes_in_imap = EXT4_WRITE_BUF_SIZE;

    for (g = 0; g < fs->bgd_count; g++) {
        uint8_t *bgd_entry = scratch_bgd + (uint32_t)g * fs->bgd_size;
        uint16_t free_lo   = le16(bgd_entry + 0x0E);
        uint32_t free_hi   = (fs->bgd_size >= 64u) ? le16(bgd_entry + 0x2E) : 0u;
        uint8_t  bg_flags  = bgd_entry[0x12];
        uint64_t ibmap_phys;
        int      found = 0;

        if (free_lo == 0u && free_hi == 0u) continue;

        ibmap_phys = ((fs->bgd_size >= 64u)
                      ? ((uint64_t)le32(bgd_entry + 0x24) << 32) : 0u)
                   | (uint64_t)le32(bgd_entry + 0x04);

        if (bg_flags & 0x2u) {
            /* INODE_UNINIT: all inodes free; generate all-zero bitmap. */
            memset(scratch_bitmap, 0, fs->sb.block_size);
        } else {
            if (ext4_fs_read_block(fs, ibmap_phys, scratch_bitmap) != 0) continue;
        }

        for (bit = 0; bit < bytes_in_imap * 8u; bit++) {
            uint32_t b = bit >> 3;
            uint8_t  m = (uint8_t)(1u << (bit & 7u));
            if (!(scratch_bitmap[b] & m)) {
                scratch_bitmap[b] |= m;
                alloc_inode_num   = g * fs->sb.inodes_per_group + bit + 1u;
                alloc_inode_group = g;
                found = 1;
                break;
            }
        }
        if (!found) continue;

        /* Clear INODE_UNINIT if we initialised the bitmap ourselves. */
        if (bg_flags & 0x2u) bgd_entry[0x12] &= ~(uint8_t)0x2u;

        /* Decrement free_inodes_count in BGD entry. */
        if (free_lo == 0u) { free_hi -= 1u; free_lo = 0xFFFFu; }
        else                { free_lo -= 1u; }
        bgd_entry[0x0E] = (uint8_t)free_lo;
        bgd_entry[0x0F] = (uint8_t)(free_lo >> 8);
        if (fs->bgd_size >= 64u) {
            bgd_entry[0x2E] = (uint8_t)free_hi;
            bgd_entry[0x2F] = (uint8_t)(free_hi >> 8);
        }

        /* Update itable_unused: tracks how many inodes at the HIGH end of
         * the inode table have never been allocated. When we allocate inode
         * at index `bit` and it falls in that "unused" tail, shrink the
         * tail count so e2fsck doesn't complain about "inode in unused area". */
        {
            uint16_t iu_lo = le16(bgd_entry + 0x1C);
            uint32_t iu_hi = (fs->bgd_size >= 64u) ? le16(bgd_entry + 0x32) : 0u;
            uint32_t itable_unused = ((uint32_t)iu_hi << 16) | iu_lo;
            uint32_t unused_start  = fs->sb.inodes_per_group - itable_unused;
            if (bit >= unused_start && itable_unused > 0u) {
                /* New inode is in the "unused tail" — shrink the tail so
                 * that all inodes up to `bit` inclusive are tracked. */
                uint32_t new_unused = (bit + 1u >= fs->sb.inodes_per_group)
                                      ? 0u
                                      : (fs->sb.inodes_per_group - (bit + 1u));
                iu_lo = (uint16_t)(new_unused & 0xFFFFu);
                iu_hi = (uint16_t)(new_unused >> 16);
                bgd_entry[0x1C] = (uint8_t) iu_lo;
                bgd_entry[0x1D] = (uint8_t)(iu_lo >> 8);
                if (fs->bgd_size >= 64u) {
                    bgd_entry[0x32] = (uint8_t) iu_hi;
                    bgd_entry[0x33] = (uint8_t)(iu_hi >> 8);
                }
            }
        }

        bgd_finalize_inode_bitmap_csums(fs, g, bgd_entry, scratch_bitmap);
        return 0;
    }

    say_simple(err, err_len, "no free inode in any group");
    return -1;
}

uint32_t ext4_file_create(struct ext4_fs *fs, uint32_t parent_ino,
                          const char *name, uint8_t name_len,
                          uint16_t mode, uint32_t now_unix,
                          char *err, uint32_t err_len) {
    /* Uses shared scratch buffers: bitmap=inode bitmap, bgd=BGD,
     * sb=fs SB, inode_block=new inode block, data=parent dir block. */
    static struct ext4_inode parent_inode;

    uint64_t   bgd_fs_block, sb_fs_block;
    uint32_t   sb_offset_in_block;
    uint8_t   *sb_in_block;
    uint32_t   parent_gen;
    uint32_t   inode_group, inode_idx_in_group;
    uint64_t   inode_table_block, inode_byte, inode_fs_block;
    uint32_t   inode_offset;
    uint64_t   dir_fs_block;           /* parent dir data block we'll modify */
    static uint64_t dir_phys;          /* DGROUP — extent_lookup output */
    uint8_t   *dir_blk;                /* = scratch_data */
    uint32_t   new_rec_needed;         /* min bytes for new entry */
    int        found_slot = 0;
    uint32_t   slot_off = 0;           /* where to write new entry in dir block */
    int        rc;

    if (err && err_len) err[0] = '\0';
    if (fs->sb.block_size > EXT4_WRITE_BUF_SIZE) {
        say_simple(err, err_len, "block_size > write cap (DOS DGROUP)");
        return 0;
    }
    if (name_len == 0) {
        say_simple(err, err_len, "name length 0");
        return 0;
    }

    /* Round up new entry size to 4-byte boundary: 8 bytes header + name. */
    new_rec_needed = (8u + (uint32_t)name_len + 3u) & ~(uint32_t)3u;

    /* --- Read parent inode to check for htree + get extent header --- */
    rc = ext4_inode_read(fs, parent_ino, &parent_inode);
    if (rc) { say_simple(err, err_len, "parent inode read failed"); return 0; }

    /* Refuse htree directories (EXT2_INDEX_FL = 0x1000 in i_flags). */
    if (parent_inode.flags & 0x1000u) {
        say_simple(err, err_len, "htree dir — only linear dirs supported");
        return 0;
    }

    /* Get parent_gen from raw inode: offset 0x64 in inode bytes.
     * Read the inode FS block temporarily into scratch_inode_block. */
    {
        const uint8_t *bgd0;
        uint32_t       g0, idx0;
        g0   = (parent_ino - 1u) / fs->sb.inodes_per_group;
        idx0 = (parent_ino - 1u) % fs->sb.inodes_per_group;
        bgd0 = fs->bgd_buf + (uint32_t)g0 * fs->bgd_size;
        {
            uint32_t lo = le32(bgd0 + 0x08);
            uint32_t hi = (fs->bgd_size >= 64u) ? le32(bgd0 + 0x28) : 0u;
            inode_table_block = ((uint64_t)hi << 32) | lo;
        }
        inode_byte     = (uint64_t)idx0 * fs->sb.inode_size;
        inode_fs_block = inode_table_block + inode_byte / fs->sb.block_size;
        inode_offset   = (uint32_t)(inode_byte % fs->sb.block_size);
        rc = ext4_fs_read_block(fs, inode_fs_block, scratch_inode_block);
        if (rc) { say_simple(err, err_len, "parent inode block read failed"); return 0; }
        parent_gen = le32(scratch_inode_block + inode_offset + 0x64);
    }

    /* --- Scan parent dir blocks for an entry slot --- */
    dir_blk = scratch_data;
    {
        uint32_t nblocks = (uint32_t)((parent_inode.size + fs->sb.block_size - 1u)
                                      / fs->sb.block_size);
        uint32_t b;
        for (b = 0; b < nblocks && !found_slot; b++) {
            rc = ext4_extent_lookup(fs, parent_inode.i_block, b, &dir_phys);
            if (rc) continue;
            dir_fs_block = dir_phys;
            rc = ext4_fs_read_block(fs, dir_fs_block, dir_blk);
            if (rc) continue;
            found_slot = dir_find_slot_for_entry(dir_blk, fs->sb.block_size,
                                                 new_rec_needed);
            if (found_slot) slot_off = s_dir_slot_off;
        }
    }
    if (!found_slot) {
        say_simple(err, err_len, "no room in parent dir");
        return 0;
    }

    /* --- Allocate a free inode --- */
    if (find_free_inode(fs, err, err_len) != 0) return 0;

    /* --- Write new dir entry into dir_blk at slot_off --- */
    {
        uint32_t remaining = s_dir_slot_rec;
        dir_blk[slot_off + 0] = (uint8_t) alloc_inode_num;
        dir_blk[slot_off + 1] = (uint8_t)(alloc_inode_num >>  8);
        dir_blk[slot_off + 2] = (uint8_t)(alloc_inode_num >> 16);
        dir_blk[slot_off + 3] = (uint8_t)(alloc_inode_num >> 24);
        dir_blk[slot_off + 4] = (uint8_t) remaining;
        dir_blk[slot_off + 5] = (uint8_t)(remaining >> 8);
        dir_blk[slot_off + 6] = name_len;
        dir_blk[slot_off + 7] = (fs->sb.feature_incompat & 0x2u) ? 1u : 0u; /* EXT4_FT_REGULAR=1 */
        memcpy(dir_blk + slot_off + 8u, name, name_len);
    }
    dir_block_set_tail_csum(fs, dir_blk, parent_ino, parent_gen);

    /* --- Read SB block + decrement free_inodes_count --- */
    bgd_fs_block        = (fs->sb.block_size > 1024u) ? 1u : 2u;
    sb_fs_block         = (fs->sb.block_size > 1024u) ? 0u : 1u;
    sb_offset_in_block  = (fs->sb.block_size > 1024u) ? 1024u : 0u;
    rc = ext4_fs_read_block(fs, sb_fs_block, scratch_sb);
    if (rc) { say_simple(err, err_len, "fs sb block read failed"); return 0; }
    sb_in_block = scratch_sb + sb_offset_in_block;
    {
        uint32_t fi = le32(sb_in_block + 0x10);
        if (fi == 0u) { say_simple(err, err_len, "fs sb: no free inodes"); return 0; }
        fi -= 1u;
        sb_in_block[0x10] = (uint8_t) fi;
        sb_in_block[0x11] = (uint8_t)(fi >>  8);
        sb_in_block[0x12] = (uint8_t)(fi >> 16);
        sb_in_block[0x13] = (uint8_t)(fi >> 24);
    }
    sb_recompute_csum(fs, sb_in_block);

    /* --- Initialize new inode block --- */
    inode_group         = (alloc_inode_num - 1u) / fs->sb.inodes_per_group;
    inode_idx_in_group  = (alloc_inode_num - 1u) % fs->sb.inodes_per_group;
    {
        const uint8_t *bgd0 = fs->bgd_buf + (uint32_t)inode_group * fs->bgd_size;
        uint32_t lo = le32(bgd0 + 0x08);
        uint32_t hi = (fs->bgd_size >= 64u) ? le32(bgd0 + 0x28) : 0u;
        inode_table_block = ((uint64_t)hi << 32) | lo;
    }
    inode_byte     = (uint64_t)inode_idx_in_group * fs->sb.inode_size;
    inode_fs_block = inode_table_block + inode_byte / fs->sb.block_size;
    inode_offset   = (uint32_t)(inode_byte % fs->sb.block_size);

    rc = ext4_fs_read_block(fs, inode_fs_block, scratch_inode_block);
    if (rc) { say_simple(err, err_len, "new inode block read failed"); return 0; }

    /* Zero the inode slot, then fill key fields. */
    memset(scratch_inode_block + inode_offset, 0, fs->sb.inode_size);
    {
        uint8_t *in = scratch_inode_block + inode_offset;
        /* i_mode */
        in[0x00] = (uint8_t)mode; in[0x01] = (uint8_t)(mode >> 8);
        /* i_links_count = 1 */
        in[0x1A] = 1u; in[0x1B] = 0u;
        /* i_flags = EXT4_INODE_FLAG_EXTENTS */
        in[0x20] = (uint8_t)EXT4_INODE_FLAG_EXTENTS;
        in[0x21] = (uint8_t)(EXT4_INODE_FLAG_EXTENTS >>  8);
        in[0x22] = (uint8_t)(EXT4_INODE_FLAG_EXTENTS >> 16);
        in[0x23] = (uint8_t)(EXT4_INODE_FLAG_EXTENTS >> 24);
        /* i_atime, i_ctime, i_mtime = now_unix */
        in[0x08] = in[0x0C] = in[0x10] = (uint8_t) now_unix;
        in[0x09] = in[0x0D] = in[0x11] = (uint8_t)(now_unix >>  8);
        in[0x0A] = in[0x0E] = in[0x12] = (uint8_t)(now_unix >> 16);
        in[0x0B] = in[0x0F] = in[0x13] = (uint8_t)(now_unix >> 24);
        /* i_block: extent header (magic, entries=0, max=4, depth=0, gen=0). */
        in[0x28] = (uint8_t)EXT4_EXT_MAGIC; in[0x29] = (uint8_t)(EXT4_EXT_MAGIC >> 8);
        in[0x2A] = 0u; in[0x2B] = 0u;  /* entries = 0 */
        in[0x2C] = 4u; in[0x2D] = 0u;  /* max = 4 */
        /* depth, generation = 0 (already zeroed) */
        /* i_extra_isize if large inode */
        if (fs->sb.inode_size > 128u) {
            uint16_t extra = (uint16_t)(fs->sb.inode_size - 128u);
            in[0x80] = (uint8_t) extra;
            in[0x81] = (uint8_t)(extra >> 8);
        }
        ext4_inode_recompute_csum(fs, alloc_inode_num, in);
    }

    /* --- Commit 5-block transaction --- */
    scratch_trans.block_count = 5u;
    scratch_trans.fs_block[0] = dir_fs_block;
    scratch_trans.buf[0]      = scratch_data;
    scratch_trans.fs_block[1] = (((fs->bgd_size >= 64u)
                                   ? ((uint64_t)le32(scratch_bgd
                                      + alloc_inode_group * fs->bgd_size + 0x24) << 32)
                                   : 0u)
                                  | (uint64_t)le32(scratch_bgd
                                      + alloc_inode_group * fs->bgd_size + 0x04));
    scratch_trans.buf[1]      = scratch_bitmap;
    scratch_trans.fs_block[2] = bgd_fs_block;
    scratch_trans.buf[2]      = scratch_bgd;
    scratch_trans.fs_block[3] = sb_fs_block;
    scratch_trans.buf[3]      = scratch_sb;
    scratch_trans.fs_block[4] = inode_fs_block;
    scratch_trans.buf[4]      = scratch_inode_block;

    rc = ext4_journal_commit(fs, &scratch_trans, err, err_len);
    if (rc) return 0;

    return alloc_inode_num;
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

        bgd_finalize_block_bitmap_csums(fs, g, bgd_entry, scratch_bitmap);
        return 0;
    }

    say_simple(err, err_len, "no free block in any group");
    return -1;
}

int ext4_file_extend_block(struct ext4_fs *fs, struct ext4_inode *inode_in,
                           uint32_t inode_num, const void *new_data,
                           uint32_t append_bytes,
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
    if (ext_entries > 4) {
        say_simple(err, err_len, "extent entries >4 — defer");
        return -1;
    }
    if (ext_entries == 0) {
        /* Empty file — first write. No last leaf to extend; we'll append
         * the very first leaf entry. ext_phys=0 ensures non-contiguous. */
        if (inode_in->size != 0u) {
            say_simple(err, err_len, "ext_entries=0 but size!=0 — inconsistent");
            return -1;
        }
        ext_logical = 0u; ext_len = 0u; ext_phys = 0u;
    } else {
        const uint8_t *e = iblock + 12 + ((uint32_t)ext_entries - 1u) * 12u;
        ext_logical = le32(e + 0);
        ext_len     = le16(e + 4);
        if (ext_len & 0x8000u) {
            say_simple(err, err_len, "uninitialized extent — not handled");
            return -1;
        }
        ext_phys    = ((uint64_t)le16(e + 6) << 32) | (uint64_t)le32(e + 8);
        if ((uint64_t)inode_in->size !=
            (uint64_t)(ext_logical + ext_len) * fs->sb.block_size) {
            say_simple(err, err_len, "inode size doesn't match last extent end");
            return -1;
        }
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
                bgd_finalize_block_bitmap_csums(fs, target_group, bgd_entry, scratch_bitmap);
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

    sb_recompute_csum(fs, sb_in_block);

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

    /* Bump i_size_lo and i_size_hi by block_size. Sub-4GB files are
     * the common case, so size_hi typically stays 0. */
    new_size_total = (uint64_t)inode_in->size + (uint64_t)append_bytes;
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

    /* mtime + i_checksum — must be last (after all other field writes). */
    inode_set_mtime_and_csum(fs, inode_num, inode_in_block, now_unix);

    /* --- Stage data block + commit 5-block transaction --- */
    if (append_bytes > fs->sb.block_size) append_bytes = fs->sb.block_size;
    memset(scratch_data, 0, fs->sb.block_size);
    memcpy(scratch_data, new_data, append_bytes);

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

/* --- Directory creation -------------------------------------------------- */

uint32_t ext4_dir_create(struct ext4_fs *fs, uint32_t parent_ino,
                         const char *name, uint8_t name_len,
                         uint32_t now_unix,
                         char *err, uint32_t err_len) {
    static struct ext4_inode parent_inode;
    static uint64_t dir_phys;       /* DGROUP — extent_lookup out-pointer */

    /* Locals that cross the tx1/tx2 boundary (all scalar, ~50 bytes). */
    uint32_t new_rec_needed;
    uint32_t parent_gen;
    uint64_t dir_fs_block;
    uint32_t slot_off;
    int      found_slot;
    uint64_t sb_fs_block;
    uint32_t sb_offset_in_block;
    uint64_t bgd_fs_block;
    uint64_t new_inode_fs_block;
    uint32_t new_inode_offset;
    uint64_t parent_inode_fs_block;
    uint32_t parent_inode_offset;
    uint32_t new_ino;
    int      rc;

    if (err && err_len) err[0] = '\0';
    if (fs->sb.block_size > EXT4_WRITE_BUF_SIZE) {
        say_simple(err, err_len, "block_size > write cap (DOS DGROUP)");
        return 0;
    }
    if (name_len == 0) {
        say_simple(err, err_len, "name length 0");
        return 0;
    }

    new_rec_needed = (8u + (uint32_t)name_len + 3u) & ~(uint32_t)3u;

    /* --- Read parent inode, reject htree --- */
    rc = ext4_inode_read(fs, parent_ino, &parent_inode);
    if (rc) { say_simple(err, err_len, "parent inode read failed"); return 0; }
    if (parent_inode.flags & 0x1000u) {
        say_simple(err, err_len, "htree dir — only linear dirs supported");
        return 0;
    }

    /* --- Get parent generation from raw inode bytes --- */
    {
        uint32_t g0   = (parent_ino - 1u) / fs->sb.inodes_per_group;
        uint32_t idx0 = (parent_ino - 1u) % fs->sb.inodes_per_group;
        const uint8_t *bgd0 = fs->bgd_buf + g0 * fs->bgd_size;
        uint64_t itab = ((uint64_t)((fs->bgd_size >= 64u)
                                    ? le32(bgd0 + 0x28) : 0u) << 32)
                      | (uint64_t)le32(bgd0 + 0x08);
        uint64_t ibyte  = (uint64_t)idx0 * fs->sb.inode_size;
        uint64_t iblock = itab + ibyte / fs->sb.block_size;
        uint32_t ioff   = (uint32_t)(ibyte % fs->sb.block_size);
        if (ext4_fs_read_block(fs, iblock, scratch_inode_block) != 0) {
            say_simple(err, err_len, "parent inode block read failed");
            return 0;
        }
        parent_gen = le32(scratch_inode_block + ioff + 0x64);
    }

    /* --- Scan parent dir blocks for a free entry slot --- */
    {
        uint32_t nblks = (uint32_t)((parent_inode.size
                                     + fs->sb.block_size - 1u)
                                    / fs->sb.block_size);
        uint32_t b;
        found_slot = 0;
        slot_off   = 0;
        dir_fs_block = 0;
        for (b = 0; b < nblks && !found_slot; b++) {
            rc = ext4_extent_lookup(fs, parent_inode.i_block, b, &dir_phys);
            if (rc) continue;
            dir_fs_block = dir_phys;
            if (ext4_fs_read_block(fs, dir_fs_block, scratch_parent_dir) != 0)
                continue;
            found_slot = dir_find_slot_for_entry(scratch_parent_dir,
                                                 fs->sb.block_size,
                                                 new_rec_needed);
            if (found_slot) slot_off = s_dir_slot_off;
        }
    }
    if (!found_slot) {
        say_simple(err, err_len, "no room in parent dir");
        return 0;
    }

    /* Cache SB/BGD block addresses (same for both transactions). */
    bgd_fs_block       = (fs->sb.block_size > 1024u) ? 1u : 2u;
    sb_fs_block        = (fs->sb.block_size > 1024u) ? 0u : 1u;
    sb_offset_in_block = (fs->sb.block_size > 1024u) ? 1024u : 0u;

    /* ============================================================
     * Transaction 1: allocate the inode.
     * ============================================================ */
    if (find_free_inode(fs, err, err_len) != 0) return 0;
    new_ino = alloc_inode_num;

    /* Increment bg_used_dirs_count for the group hosting the new inode.
     * find_free_inode already decremented free_inodes and recomputed the
     * BGD csum — we update used_dirs and recompute csum again. */
    {
        uint8_t *bgd_entry = scratch_bgd + alloc_inode_group * fs->bgd_size;
        uint16_t ud_lo = le16(bgd_entry + 0x10);
        uint16_t ud_hi = (fs->bgd_size >= 64u) ? le16(bgd_entry + 0x30) : 0u;
        ud_lo++;
        if (ud_lo == 0u) ud_hi++;
        bgd_entry[0x10] = (uint8_t)ud_lo; bgd_entry[0x11] = (uint8_t)(ud_lo >> 8);
        if (fs->bgd_size >= 64u) {
            bgd_entry[0x30] = (uint8_t)ud_hi; bgd_entry[0x31] = (uint8_t)(ud_hi >> 8);
        }
        {
            uint16_t bc = bgd_compute_csum(fs, alloc_inode_group, bgd_entry);
            bgd_entry[0x1E] = (uint8_t)bc; bgd_entry[0x1F] = (uint8_t)(bc >> 8);
        }
    }

    /* SB: decrement free_inodes_count. */
    if (ext4_fs_read_block(fs, sb_fs_block, scratch_sb) != 0) {
        say_simple(err, err_len, "sb read failed (tx1)"); return 0;
    }
    {
        uint8_t *sb = scratch_sb + sb_offset_in_block;
        uint32_t fi = le32(sb + 0x10);
        if (fi == 0u) { say_simple(err, err_len, "no free inodes in sb"); return 0; }
        fi--;
        sb[0x10] = (uint8_t) fi; sb[0x11] = (uint8_t)(fi >> 8);
        sb[0x12] = (uint8_t)(fi >> 16); sb[0x13] = (uint8_t)(fi >> 24);
        sb_recompute_csum(fs, sb);
    }

    /* Locate new dir's inode block (using bgd_buf — inode table never moves). */
    {
        uint32_t g   = (new_ino - 1u) / fs->sb.inodes_per_group;
        uint32_t idx = (new_ino - 1u) % fs->sb.inodes_per_group;
        const uint8_t *bgd0 = fs->bgd_buf + g * fs->bgd_size;
        uint64_t itab = ((uint64_t)((fs->bgd_size >= 64u)
                                    ? le32(bgd0 + 0x28) : 0u) << 32)
                      | (uint64_t)le32(bgd0 + 0x08);
        uint64_t ibyte = (uint64_t)idx * fs->sb.inode_size;
        new_inode_fs_block = itab + ibyte / fs->sb.block_size;
        new_inode_offset   = (uint32_t)(ibyte % fs->sb.block_size);
    }
    if (ext4_fs_read_block(fs, new_inode_fs_block, scratch_inode_block) != 0) {
        say_simple(err, err_len, "new dir inode block read failed"); return 0;
    }
    memset(scratch_inode_block + new_inode_offset, 0, fs->sb.inode_size);
    {
        uint8_t *in = scratch_inode_block + new_inode_offset;
        uint16_t mode = (uint16_t)(EXT4_S_IFDIR | 0755u);
        in[0x00] = (uint8_t)mode; in[0x01] = (uint8_t)(mode >> 8);
        in[0x1A] = 2u; in[0x1B] = 0u; /* links = 2 (. + parent entry) */
        in[0x20] = (uint8_t)EXT4_INODE_FLAG_EXTENTS;
        in[0x21] = (uint8_t)(EXT4_INODE_FLAG_EXTENTS >>  8);
        in[0x22] = (uint8_t)(EXT4_INODE_FLAG_EXTENTS >> 16);
        in[0x23] = (uint8_t)(EXT4_INODE_FLAG_EXTENTS >> 24);
        in[0x08] = in[0x0C] = in[0x10] = (uint8_t) now_unix;
        in[0x09] = in[0x0D] = in[0x11] = (uint8_t)(now_unix >>  8);
        in[0x0A] = in[0x0E] = in[0x12] = (uint8_t)(now_unix >> 16);
        in[0x0B] = in[0x0F] = in[0x13] = (uint8_t)(now_unix >> 24);
        in[0x28] = (uint8_t)EXT4_EXT_MAGIC; in[0x29] = (uint8_t)(EXT4_EXT_MAGIC >> 8);
        in[0x2C] = 4u; /* max = 4 */
        if (fs->sb.inode_size > 128u) {
            uint16_t xs = (uint16_t)(fs->sb.inode_size - 128u);
            in[0x80] = (uint8_t)xs; in[0x81] = (uint8_t)(xs >> 8);
        }
        ext4_inode_recompute_csum(fs, new_ino, in);
    }
    scratch_trans.block_count = 4u;
    scratch_trans.fs_block[0] = (((fs->bgd_size >= 64u)
                                   ? ((uint64_t)le32(scratch_bgd
                                      + alloc_inode_group * fs->bgd_size + 0x24) << 32)
                                   : 0u)
                                  | (uint64_t)le32(scratch_bgd
                                      + alloc_inode_group * fs->bgd_size + 0x04));
    scratch_trans.buf[0]      = scratch_bitmap;
    scratch_trans.fs_block[1] = bgd_fs_block;
    scratch_trans.buf[1]      = scratch_bgd;
    scratch_trans.fs_block[2] = sb_fs_block;
    scratch_trans.buf[2]      = scratch_sb;
    scratch_trans.fs_block[3] = new_inode_fs_block;
    scratch_trans.buf[3]      = scratch_inode_block;
    rc = ext4_journal_commit(fs, &scratch_trans, err, err_len);
    if (rc) return 0;

    /* ============================================================
     * Transaction 2: allocate data block + wire everything up.
     * find_free_block reloads scratch_bitmap/scratch_bgd from disk
     * (now reflecting tx1's changes) before modifying the block bitmap.
     * ============================================================ */
    if (find_free_block(fs, err, err_len) != 0) return 0;

    /* SB: decrement free_blocks_count. */
    if (ext4_fs_read_block(fs, sb_fs_block, scratch_sb) != 0) {
        say_simple(err, err_len, "sb read failed (tx2)"); return 0;
    }
    {
        uint8_t *sb = scratch_sb + sb_offset_in_block;
        uint32_t lo = le32(sb + 0x0C);
        uint32_t hi = (fs->sb.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
                      ? le32(sb + 0x158) : 0u;
        uint64_t fb = ((uint64_t)hi << 32) | lo;
        if (fb == 0u) { say_simple(err, err_len, "no free blocks in sb"); return 0; }
        fb--;
        sb[0x0C] = (uint8_t)fb; sb[0x0D] = (uint8_t)(fb >> 8);
        sb[0x0E] = (uint8_t)(fb >> 16); sb[0x0F] = (uint8_t)(fb >> 24);
        if (fs->sb.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
            uint32_t h32 = (uint32_t)(fb >> 32);
            sb[0x158] = (uint8_t)h32; sb[0x159] = (uint8_t)(h32 >> 8);
            sb[0x15A] = (uint8_t)(h32 >> 16); sb[0x15B] = (uint8_t)(h32 >> 24);
        }
        sb_recompute_csum(fs, sb);
    }

    /* Build new dir data block (. and ..) in scratch_data. */
    memset(scratch_data, 0, fs->sb.block_size);
    {
        uint8_t  ft_dir = (fs->sb.feature_incompat & 0x2u) ? 2u : 0u;
        uint32_t dotdot_rec = fs->sb.block_size - 12u;
        if (fs->sb.feature_ro_compat & 0x400u) dotdot_rec -= 12u;
        /* . */
        scratch_data[0] = (uint8_t)new_ino;
        scratch_data[1] = (uint8_t)(new_ino >>  8);
        scratch_data[2] = (uint8_t)(new_ino >> 16);
        scratch_data[3] = (uint8_t)(new_ino >> 24);
        scratch_data[4] = 12u; scratch_data[5] = 0u;
        scratch_data[6] = 1u;  scratch_data[7] = ft_dir;
        scratch_data[8] = '.';
        /* .. */
        scratch_data[12] = (uint8_t)parent_ino;
        scratch_data[13] = (uint8_t)(parent_ino >>  8);
        scratch_data[14] = (uint8_t)(parent_ino >> 16);
        scratch_data[15] = (uint8_t)(parent_ino >> 24);
        scratch_data[16] = (uint8_t) dotdot_rec;
        scratch_data[17] = (uint8_t)(dotdot_rec >> 8);
        scratch_data[18] = 2u;  scratch_data[19] = ft_dir;
        scratch_data[20] = '.'; scratch_data[21] = '.';
        dir_block_set_tail_csum(fs, scratch_data, new_ino, 0u);
    }

    /* Insert new dir entry into parent's dir block (scratch_parent_dir).
     * Scan already found slot_off and shrunk the predecessor; the
     * new entry's rec_len is exactly the predecessor's freed dead-space. */
    {
        uint32_t remaining = s_dir_slot_rec;
        uint8_t  ft_dir    = (fs->sb.feature_incompat & 0x2u) ? 2u : 0u;
        scratch_parent_dir[slot_off + 0] = (uint8_t)new_ino;
        scratch_parent_dir[slot_off + 1] = (uint8_t)(new_ino >>  8);
        scratch_parent_dir[slot_off + 2] = (uint8_t)(new_ino >> 16);
        scratch_parent_dir[slot_off + 3] = (uint8_t)(new_ino >> 24);
        scratch_parent_dir[slot_off + 4] = (uint8_t) remaining;
        scratch_parent_dir[slot_off + 5] = (uint8_t)(remaining >> 8);
        scratch_parent_dir[slot_off + 6] = name_len;
        scratch_parent_dir[slot_off + 7] = ft_dir;
        memcpy(scratch_parent_dir + slot_off + 8u, name, name_len);
    }
    dir_block_set_tail_csum(fs, scratch_parent_dir, parent_ino, parent_gen);

    /* Re-read new dir's inode block (tx1 committed it), add extent + size. */
    if (ext4_fs_read_block(fs, new_inode_fs_block, scratch_inode_block) != 0) {
        say_simple(err, err_len, "new dir inode re-read failed"); return 0;
    }
    {
        uint8_t *in = scratch_inode_block + new_inode_offset;
        uint8_t *e  = in + 0x28 + 12u; /* first extent slot */
        uint32_t bs = fs->sb.block_size;
        in[0x2A] = 1u; in[0x2B] = 0u; /* entries = 1 */
        e[0] = e[1] = e[2] = e[3] = 0u; /* ee_block = 0 */
        e[4] = 1u; e[5] = 0u;            /* ee_len = 1 */
        e[6]  = (uint8_t)(uint16_t)(alloc_candidate >> 32);
        e[7]  = (uint8_t)((uint16_t)(alloc_candidate >> 32) >> 8);
        e[8]  = (uint8_t) alloc_candidate;
        e[9]  = (uint8_t)(alloc_candidate >>  8);
        e[10] = (uint8_t)(alloc_candidate >> 16);
        e[11] = (uint8_t)(alloc_candidate >> 24);
        in[0x04] = (uint8_t)bs; in[0x05] = (uint8_t)(bs >>  8); /* size */
        in[0x06] = (uint8_t)(bs >> 16); in[0x07] = (uint8_t)(bs >> 24);
        { /* i_blocks_lo in 512-byte units */
            uint32_t blk_sec = bs / 512u;
            in[0x1C] = (uint8_t)blk_sec; in[0x1D] = (uint8_t)(blk_sec >> 8);
            in[0x1E] = (uint8_t)(blk_sec >> 16); in[0x1F] = (uint8_t)(blk_sec >> 24);
        }
        ext4_inode_recompute_csum(fs, new_ino, in);
    }

    /* Read parent inode block, bump i_links_count + mtime. */
    {
        uint32_t g   = (parent_ino - 1u) / fs->sb.inodes_per_group;
        uint32_t idx = (parent_ino - 1u) % fs->sb.inodes_per_group;
        const uint8_t *bgd0 = fs->bgd_buf + g * fs->bgd_size;
        uint64_t itab = ((uint64_t)((fs->bgd_size >= 64u)
                                    ? le32(bgd0 + 0x28) : 0u) << 32)
                      | (uint64_t)le32(bgd0 + 0x08);
        uint64_t ibyte = (uint64_t)idx * fs->sb.inode_size;
        parent_inode_fs_block = itab + ibyte / fs->sb.block_size;
        parent_inode_offset   = (uint32_t)(ibyte % fs->sb.block_size);
    }
    if (ext4_fs_read_block(fs, parent_inode_fs_block, scratch_parent_inode) != 0) {
        say_simple(err, err_len, "parent inode block read failed (tx2)"); return 0;
    }
    {
        uint8_t  *pi = scratch_parent_inode + parent_inode_offset;
        uint16_t  lc = le16(pi + 0x1A);
        lc++;
        pi[0x1A] = (uint8_t)lc; pi[0x1B] = (uint8_t)(lc >> 8);
        inode_set_mtime_and_csum(fs, parent_ino, pi, now_unix);
    }

    scratch_trans.block_count = 7u;
    scratch_trans.fs_block[0] = alloc_candidate;   /* new dir data block */
    scratch_trans.buf[0]      = scratch_data;
    scratch_trans.fs_block[1] = alloc_bitmap_phys; /* block bitmap */
    scratch_trans.buf[1]      = scratch_bitmap;
    scratch_trans.fs_block[2] = bgd_fs_block;
    scratch_trans.buf[2]      = scratch_bgd;
    scratch_trans.fs_block[3] = sb_fs_block;
    scratch_trans.buf[3]      = scratch_sb;
    scratch_trans.fs_block[4] = new_inode_fs_block; /* new dir inode + extent */
    scratch_trans.buf[4]      = scratch_inode_block;
    scratch_trans.fs_block[5] = dir_fs_block;       /* parent dir + new entry */
    scratch_trans.buf[5]      = scratch_parent_dir;
    scratch_trans.fs_block[6] = parent_inode_fs_block; /* parent nlinks++ */
    scratch_trans.buf[6]      = scratch_parent_inode;
    rc = ext4_journal_commit(fs, &scratch_trans, err, err_len);
    if (rc) return 0;

    return new_ino;
}

/* --- Directory removal --------------------------------------------------- */

/* Physical address of the directory's data block, saved across tx1/tx2. */
static uint64_t rmdir_data_phys;

int ext4_dir_remove(struct ext4_fs *fs, uint32_t parent_ino, uint32_t dir_ino,
                    char *err, uint32_t err_len) {
    static struct ext4_inode dir_inode;
    static struct ext4_inode par_inode;
    static uint64_t dir_phys;       /* DGROUP — extent_lookup out-pointer */

    uint64_t sb_fs_block, bgd_fs_block;
    uint32_t sb_offset_in_block;
    uint32_t parent_gen;
    uint64_t parent_dir_block;
    uint32_t parent_slot_off, parent_prev_off;
    uint32_t data_group, data_bit;
    uint64_t data_bmap_phys;
    uint32_t inode_group, inode_bit;
    uint64_t inode_bmap_phys;
    uint64_t dir_inode_fs_block;
    uint32_t dir_inode_offset;
    uint64_t parent_inode_fs_block;
    uint32_t parent_inode_offset;
    int rc;

    if (err && err_len) err[0] = '\0';
    if (fs->sb.block_size > EXT4_WRITE_BUF_SIZE) {
        say_simple(err, err_len, "block_size > write cap");
        return -1;
    }
    if (dir_ino == 0u || dir_ino == 2u) {
        say_simple(err, err_len, "cannot remove root");
        return -1;
    }

    /* --- Verify dir_ino is an empty directory --- */
    if (ext4_inode_read(fs, dir_ino, &dir_inode) != 0) {
        say_simple(err, err_len, "dir inode read failed"); return -1;
    }
    if ((dir_inode.mode & 0xF000u) != EXT4_S_IFDIR) {
        say_simple(err, err_len, "not a directory"); return -1;
    }
    {
        const uint8_t *iblk = dir_inode.i_block;
        if (le16(iblk + 0) != EXT4_EXT_MAGIC) {
            say_simple(err, err_len, "dir lacks extent header"); return -1;
        }
        if (le16(iblk + 6) != 0 || le16(iblk + 2) != 1) {
            say_simple(err, err_len, "dir extent tree not a single leaf"); return -1;
        }
    }
    rc = ext4_extent_lookup(fs, dir_inode.i_block, 0, &rmdir_data_phys);
    if (rc) { say_simple(err, err_len, "extent lookup failed"); return -1; }

    /* Read dir's data block and confirm it contains only . and .. */
    if (ext4_fs_read_block(fs, rmdir_data_phys, scratch_data) != 0) {
        say_simple(err, err_len, "dir data read failed"); return -1;
    }
    {
        uint32_t off = 0;
        while (off + 8u <= fs->sb.block_size) {
            uint16_t rec = le16(scratch_data + off + 4);
            uint8_t  nl  = scratch_data[off + 6];
            uint32_t ino = le32(scratch_data + off);
            if (rec < 8u || rec > fs->sb.block_size - off) break;
            if (scratch_data[off + 7] == 0xDEu && rec == 12u && ino == 0u) {
                off += rec; continue;                      /* tail sentinel */
            }
            if (ino == 0u) { off += rec; continue; }       /* free slot */
            if (nl == 1u && scratch_data[off + 8u] == '.') { off += rec; continue; }
            if (nl == 2u && scratch_data[off + 8u] == '.'
                         && scratch_data[off + 9u] == '.') { off += rec; continue; }
            say_simple(err, err_len, "directory not empty"); return -1;
        }
    }

    /* --- Get parent generation for dir-block tail csum --- */
    if (ext4_inode_read(fs, parent_ino, &par_inode) != 0) {
        say_simple(err, err_len, "parent inode read failed"); return -1;
    }
    {
        uint32_t g0  = (parent_ino - 1u) / fs->sb.inodes_per_group;
        uint32_t id0 = (parent_ino - 1u) % fs->sb.inodes_per_group;
        const uint8_t *b0 = fs->bgd_buf + g0 * fs->bgd_size;
        uint64_t it = ((uint64_t)((fs->bgd_size >= 64u) ? le32(b0 + 0x28) : 0u) << 32)
                    | (uint64_t)le32(b0 + 0x08);
        uint64_t ib = (uint64_t)id0 * fs->sb.inode_size;
        if (ext4_fs_read_block(fs, it + ib / fs->sb.block_size, scratch_inode_block) != 0) {
            say_simple(err, err_len, "parent inode block read"); return -1;
        }
        parent_gen = le32(scratch_inode_block + (uint32_t)(ib % fs->sb.block_size) + 0x64);
    }

    /* --- Scan parent dir for the entry pointing to dir_ino --- */
    {
        uint32_t nblks = (uint32_t)((par_inode.size + fs->sb.block_size - 1u)
                                    / fs->sb.block_size);
        uint32_t b;
        int found = 0;
        parent_slot_off = 0; parent_prev_off = 0xFFFFFFFFu; parent_dir_block = 0;
        for (b = 0; b < nblks && !found; b++) {
            if (ext4_extent_lookup(fs, par_inode.i_block, b, &dir_phys) != 0) continue;
            parent_dir_block = dir_phys;
            if (ext4_fs_read_block(fs, parent_dir_block, scratch_parent_dir) != 0) continue;
            found = dir_find_entry_by_inode(scratch_parent_dir, fs->sb.block_size, dir_ino);
            if (found) { parent_slot_off = s_dir_entry_off; parent_prev_off = s_dir_prev_off; }
        }
        if (!found) { say_simple(err, err_len, "entry not found in parent"); return -1; }
    }

    /* Remove the entry: expand predecessor's rec_len or zero the inode field. */
    {
        uint16_t cur_rec = le16(scratch_parent_dir + parent_slot_off + 4);
        if (parent_prev_off != 0xFFFFFFFFu) {
            uint16_t prev_rec = le16(scratch_parent_dir + parent_prev_off + 4);
            uint16_t new_rec  = (uint16_t)(prev_rec + cur_rec);
            scratch_parent_dir[parent_prev_off + 4] = (uint8_t) new_rec;
            scratch_parent_dir[parent_prev_off + 5] = (uint8_t)(new_rec >> 8);
        } else {
            scratch_parent_dir[parent_slot_off + 0] = scratch_parent_dir[parent_slot_off + 1] = 0;
            scratch_parent_dir[parent_slot_off + 2] = scratch_parent_dir[parent_slot_off + 3] = 0;
        }
    }
    dir_block_set_tail_csum(fs, scratch_parent_dir, parent_ino, parent_gen);

    bgd_fs_block       = (fs->sb.block_size > 1024u) ? 1u : 2u;
    sb_fs_block        = (fs->sb.block_size > 1024u) ? 0u : 1u;
    sb_offset_in_block = (fs->sb.block_size > 1024u) ? 1024u : 0u;

    /* ================================================================
     * Transaction 1: free the data block.
     * ================================================================ */
    {
        uint64_t base = rmdir_data_phys - (uint64_t)fs->sb.first_data_block;
        data_group    = (uint32_t)(base / fs->sb.blocks_per_group);
        data_bit      = (uint32_t)(base % fs->sb.blocks_per_group);
    }
    if (ext4_fs_read_block(fs, bgd_fs_block, scratch_bgd) != 0) {
        say_simple(err, err_len, "BGD read failed (tx1)"); return -1;
    }
    {
        uint8_t *be = scratch_bgd + data_group * fs->bgd_size;
        uint16_t fl; uint32_t fh;
        data_bmap_phys = (((fs->bgd_size >= 64u)
                           ? ((uint64_t)le32(be + 0x20) << 32) : 0u)
                          | (uint64_t)le32(be + 0x00));
        if (ext4_fs_read_block(fs, data_bmap_phys, scratch_bitmap) != 0) {
            say_simple(err, err_len, "block bitmap read failed"); return -1;
        }
        scratch_bitmap[data_bit >> 3] &= ~(uint8_t)(1u << (data_bit & 7u));
        fl = le16(be + 0x0C); fh = (fs->bgd_size >= 64u) ? le16(be + 0x2C) : 0u;
        fl++; if (fl == 0u) fh++;
        be[0x0C]=(uint8_t)fl; be[0x0D]=(uint8_t)(fl>>8);
        if (fs->bgd_size >= 64u) { be[0x2C]=(uint8_t)fh; be[0x2D]=(uint8_t)(fh>>8); }
        bgd_finalize_block_bitmap_csums(fs, data_group, be, scratch_bitmap);
    }
    if (ext4_fs_read_block(fs, sb_fs_block, scratch_sb) != 0) {
        say_simple(err, err_len, "SB read failed (tx1)"); return -1;
    }
    {
        uint8_t *sb = scratch_sb + sb_offset_in_block;
        uint32_t lo = le32(sb+0x0C);
        uint32_t hi = (fs->sb.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? le32(sb+0x158) : 0u;
        uint64_t fb = ((uint64_t)hi<<32)|lo; fb++;
        sb[0x0C]=(uint8_t)fb; sb[0x0D]=(uint8_t)(fb>>8);
        sb[0x0E]=(uint8_t)(fb>>16); sb[0x0F]=(uint8_t)(fb>>24);
        if (fs->sb.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
            uint32_t h32=(uint32_t)(fb>>32);
            sb[0x158]=(uint8_t)h32; sb[0x159]=(uint8_t)(h32>>8);
            sb[0x15A]=(uint8_t)(h32>>16); sb[0x15B]=(uint8_t)(h32>>24);
        }
        sb_recompute_csum(fs, sb);
    }
    scratch_trans.block_count = 3u;
    scratch_trans.fs_block[0] = data_bmap_phys; scratch_trans.buf[0] = scratch_bitmap;
    scratch_trans.fs_block[1] = bgd_fs_block;   scratch_trans.buf[1] = scratch_bgd;
    scratch_trans.fs_block[2] = sb_fs_block;    scratch_trans.buf[2] = scratch_sb;
    rc = ext4_journal_commit(fs, &scratch_trans, err, err_len);
    if (rc) return -1;

    /* ================================================================
     * Transaction 2: free the inode + remove parent entry + update parent.
     * ================================================================ */
    inode_group = (dir_ino - 1u) / fs->sb.inodes_per_group;
    inode_bit   = (dir_ino - 1u) % fs->sb.inodes_per_group;

    if (ext4_fs_read_block(fs, bgd_fs_block, scratch_bgd) != 0) {
        say_simple(err, err_len, "BGD read failed (tx2)"); return -1;
    }
    {
        uint8_t *be = scratch_bgd + inode_group * fs->bgd_size;
        uint16_t fl, ud, uh; uint32_t fh;
        inode_bmap_phys = (((fs->bgd_size >= 64u)
                            ? ((uint64_t)le32(be + 0x24) << 32) : 0u)
                           | (uint64_t)le32(be + 0x04));
        if (ext4_fs_read_block(fs, inode_bmap_phys, scratch_bitmap) != 0) {
            say_simple(err, err_len, "inode bitmap read failed"); return -1;
        }
        scratch_bitmap[inode_bit >> 3] &= ~(uint8_t)(1u << (inode_bit & 7u));
        /* free_inodes++ */
        fl = le16(be+0x0E); fh = (fs->bgd_size>=64u) ? le16(be+0x2E) : 0u;
        fl++; if (fl==0u) fh++;
        be[0x0E]=(uint8_t)fl; be[0x0F]=(uint8_t)(fl>>8);
        if (fs->bgd_size>=64u){be[0x2E]=(uint8_t)fh; be[0x2F]=(uint8_t)(fh>>8);}
        /* used_dirs-- */
        ud = le16(be+0x10); uh = (fs->bgd_size>=64u) ? le16(be+0x30) : 0u;
        if (ud > 0u) ud--;
        else if (uh > 0u) { uh--; ud = 0xFFFFu; }
        be[0x10]=(uint8_t)ud; be[0x11]=(uint8_t)(ud>>8);
        if (fs->bgd_size>=64u){be[0x30]=(uint8_t)uh; be[0x31]=(uint8_t)(uh>>8);}
        bgd_finalize_inode_bitmap_csums(fs, inode_group, be, scratch_bitmap);
    }
    if (ext4_fs_read_block(fs, sb_fs_block, scratch_sb) != 0) {
        say_simple(err, err_len, "SB read failed (tx2)"); return -1;
    }
    {
        uint8_t *sb = scratch_sb + sb_offset_in_block;
        uint32_t fi = le32(sb+0x10); fi++;
        sb[0x10]=(uint8_t)fi; sb[0x11]=(uint8_t)(fi>>8);
        sb[0x12]=(uint8_t)(fi>>16); sb[0x13]=(uint8_t)(fi>>24);
        sb_recompute_csum(fs, sb);
    }

    /* Locate and zero the dir inode (bitmap is authoritative — zeroing
     * avoids e2fsck warnings about non-zero inodes in free bitmap slots). */
    {
        uint32_t g = inode_group, idx = inode_bit;
        const uint8_t *b0 = fs->bgd_buf + g * fs->bgd_size;
        uint64_t it = ((uint64_t)((fs->bgd_size>=64u)?le32(b0+0x28):0u)<<32)|(uint64_t)le32(b0+0x08);
        uint64_t ib = (uint64_t)idx * fs->sb.inode_size;
        dir_inode_fs_block = it + ib / fs->sb.block_size;
        dir_inode_offset   = (uint32_t)(ib % fs->sb.block_size);
    }
    if (ext4_fs_read_block(fs, dir_inode_fs_block, scratch_inode_block) != 0) {
        say_simple(err, err_len, "dir inode block read failed (tx2)"); return -1;
    }
    memset(scratch_inode_block + dir_inode_offset, 0, fs->sb.inode_size);

    /* Read parent inode block, decrement i_links_count + update mtime. */
    {
        uint32_t g = (parent_ino-1u)/fs->sb.inodes_per_group;
        uint32_t id= (parent_ino-1u)%fs->sb.inodes_per_group;
        const uint8_t *b0 = fs->bgd_buf + g * fs->bgd_size;
        uint64_t it = ((uint64_t)((fs->bgd_size>=64u)?le32(b0+0x28):0u)<<32)|(uint64_t)le32(b0+0x08);
        uint64_t ib = (uint64_t)id * fs->sb.inode_size;
        parent_inode_fs_block = it + ib / fs->sb.block_size;
        parent_inode_offset   = (uint32_t)(ib % fs->sb.block_size);
    }
    if (ext4_fs_read_block(fs, parent_inode_fs_block, scratch_parent_inode) != 0) {
        say_simple(err, err_len, "parent inode block read failed (tx2)"); return -1;
    }
    {
        uint8_t *pi = scratch_parent_inode + parent_inode_offset;
        uint16_t lc = le16(pi + 0x1A);
        if (lc > 0u) lc--;
        pi[0x1A]=(uint8_t)lc; pi[0x1B]=(uint8_t)(lc>>8);
        inode_set_mtime_and_csum(fs, parent_ino, pi, dir_inode.mtime + 1u);
    }

    scratch_trans.block_count = 6u;
    scratch_trans.fs_block[0] = inode_bmap_phys;       scratch_trans.buf[0] = scratch_bitmap;
    scratch_trans.fs_block[1] = bgd_fs_block;           scratch_trans.buf[1] = scratch_bgd;
    scratch_trans.fs_block[2] = sb_fs_block;            scratch_trans.buf[2] = scratch_sb;
    scratch_trans.fs_block[3] = dir_inode_fs_block;     scratch_trans.buf[3] = scratch_inode_block;
    scratch_trans.fs_block[4] = parent_dir_block;       scratch_trans.buf[4] = scratch_parent_dir;
    scratch_trans.fs_block[5] = parent_inode_fs_block;  scratch_trans.buf[5] = scratch_parent_inode;
    rc = ext4_journal_commit(fs, &scratch_trans, err, err_len);
    if (rc) return -1;

    return 0;
}

/* --- File removal -------------------------------------------------------- */

/* Free one allocated data block at blk_phys: clear its bit in the group's
 * block bitmap, increment that group's BGD free_blocks_count, increment
 * the SB free_blocks_count, recompute checksums, and commit a 3-block
 * journal tx (bitmap + BGD + SB). Used by ext4_file_remove and
 * ext4_file_truncate. Caller must hold no other scratch state across the
 * call. */
static int free_one_data_block(struct ext4_fs *fs, uint64_t blk_phys,
                               char *err, uint32_t err_len) {
    uint64_t base = blk_phys - (uint64_t)fs->sb.first_data_block;
    uint32_t grp  = (uint32_t)(base / fs->sb.blocks_per_group);
    uint32_t bit  = (uint32_t)(base % fs->sb.blocks_per_group);
    uint64_t bmap_phys;
    uint64_t bgd_fs_block       = (fs->sb.block_size > 1024u) ? 1u : 2u;
    uint64_t sb_fs_block        = (fs->sb.block_size > 1024u) ? 0u : 1u;
    uint32_t sb_offset_in_block = (fs->sb.block_size > 1024u) ? 1024u : 0u;
    uint8_t *be;
    uint8_t *sb;

    if (ext4_fs_read_block(fs, bgd_fs_block, scratch_bgd) != 0) {
        say_simple(err, err_len, "BGD read failed (blk free)"); return -1;
    }
    be = scratch_bgd + grp * fs->bgd_size;
    bmap_phys = (((fs->bgd_size >= 64u) ? ((uint64_t)le32(be + 0x20) << 32) : 0u)
                 | (uint64_t)le32(be + 0x00));
    if (ext4_fs_read_block(fs, bmap_phys, scratch_bitmap) != 0) {
        say_simple(err, err_len, "block bitmap read failed"); return -1;
    }
    scratch_bitmap[bit >> 3] &= ~(uint8_t)(1u << (bit & 7u));
    {
        uint16_t fl = le16(be + 0x0C);
        uint32_t fh = (fs->bgd_size >= 64u) ? le16(be + 0x2C) : 0u;
        fl++; if (fl == 0u) fh++;
        be[0x0C] = (uint8_t)fl; be[0x0D] = (uint8_t)(fl >> 8);
        if (fs->bgd_size >= 64u) {
            be[0x2C] = (uint8_t)fh; be[0x2D] = (uint8_t)(fh >> 8);
        }
    }
    bgd_finalize_block_bitmap_csums(fs, grp, be, scratch_bitmap);
    if (ext4_fs_read_block(fs, sb_fs_block, scratch_sb) != 0) {
        say_simple(err, err_len, "SB read failed (blk free)"); return -1;
    }
    sb = scratch_sb + sb_offset_in_block;
    {
        uint32_t lo = le32(sb + 0x0C);
        uint32_t hi = (fs->sb.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
                      ? le32(sb + 0x158) : 0u;
        uint64_t fb = ((uint64_t)hi << 32) | lo; fb++;
        sb[0x0C] = (uint8_t)fb; sb[0x0D] = (uint8_t)(fb >> 8);
        sb[0x0E] = (uint8_t)(fb >> 16); sb[0x0F] = (uint8_t)(fb >> 24);
        if (fs->sb.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
            uint32_t h32 = (uint32_t)(fb >> 32);
            sb[0x158] = (uint8_t)h32; sb[0x159] = (uint8_t)(h32 >> 8);
            sb[0x15A] = (uint8_t)(h32 >> 16); sb[0x15B] = (uint8_t)(h32 >> 24);
        }
        sb_recompute_csum(fs, sb);
    }
    scratch_trans.block_count = 3u;
    scratch_trans.fs_block[0] = bmap_phys;    scratch_trans.buf[0] = scratch_bitmap;
    scratch_trans.fs_block[1] = bgd_fs_block; scratch_trans.buf[1] = scratch_bgd;
    scratch_trans.fs_block[2] = sb_fs_block;  scratch_trans.buf[2] = scratch_sb;
    return ext4_journal_commit(fs, &scratch_trans, err, err_len);
}

int ext4_file_remove(struct ext4_fs *fs, uint32_t parent_ino, uint32_t file_ino,
                     char *err, uint32_t err_len) {
    static struct ext4_inode file_inode;
    static struct ext4_inode par_inode;
    static uint64_t dir_phys;       /* DGROUP — extent_lookup out-pointer */

    uint64_t sb_fs_block, bgd_fs_block;
    uint32_t sb_offset_in_block;
    uint32_t parent_gen;
    uint64_t parent_dir_block;
    uint32_t parent_slot_off, parent_prev_off;
    uint32_t inode_group, inode_bit;
    uint64_t inode_bmap_phys;
    uint64_t file_inode_fs_block;
    uint32_t file_inode_offset;
    uint64_t parent_inode_fs_block;
    uint32_t parent_inode_offset;
    int rc;

    if (err && err_len) err[0] = '\0';
    if (fs->sb.block_size > EXT4_WRITE_BUF_SIZE) {
        say_simple(err, err_len, "block_size > write cap"); return -1;
    }
    if (file_ino == 0u || file_ino == 2u) {
        say_simple(err, err_len, "cannot remove inode 0 or root"); return -1;
    }

    /* Read file inode; refuse directories (use ext4_dir_remove). */
    if (ext4_inode_read(fs, file_ino, &file_inode) != 0) {
        say_simple(err, err_len, "file inode read failed"); return -1;
    }
    if ((file_inode.mode & 0xF000u) == EXT4_S_IFDIR) {
        say_simple(err, err_len, "is a directory — use ext4_dir_remove"); return -1;
    }

    /* Verify extent tree is depth-0. */
    {
        const uint8_t *iblk = file_inode.i_block;
        if (le16(iblk + 0) != EXT4_EXT_MAGIC) {
            say_simple(err, err_len, "file lacks extent header"); return -1;
        }
        if (le16(iblk + 6) != 0) {
            say_simple(err, err_len, "extent tree depth>0"); return -1;
        }
    }

    bgd_fs_block       = (fs->sb.block_size > 1024u) ? 1u : 2u;
    sb_fs_block        = (fs->sb.block_size > 1024u) ? 0u : 1u;
    sb_offset_in_block = (fs->sb.block_size > 1024u) ? 1024u : 0u;

    /* ================================================================
     * Free each data block, batching by bitmap group.
     * For each unique group that holds data blocks: read the bitmap,
     * clear all bits belonging to that group's data blocks, update
     * BGD free_blocks_count + SB free_blocks_count, and commit.
     * ================================================================ */
    {
        const uint8_t *iblk = file_inode.i_block;
        uint16_t num_extents = le16(iblk + 2);
        uint32_t ei;

        for (ei = 0; ei < num_extents; ei++) {
            const uint8_t *e = iblk + 12u + ei * 12u;
            uint16_t len  = le16(e + 4);
            uint64_t phys = ((uint64_t)le16(e + 6) << 32) | (uint64_t)le32(e + 8);
            uint32_t bi;

            for (bi = 0; bi < (uint32_t)len; bi++) {
                if (free_one_data_block(fs, phys + (uint64_t)bi,
                                        err, err_len) != 0) return -1;
            }
        }
    }

    /* ================================================================
     * Final transaction: free inode + remove parent dir entry + update parent.
     * ================================================================ */

    /* Get parent generation for dir block csum. */
    if (ext4_inode_read(fs, parent_ino, &par_inode) != 0) {
        say_simple(err, err_len, "parent inode read failed"); return -1;
    }
    {
        uint32_t g0 = (parent_ino-1u)/fs->sb.inodes_per_group;
        uint32_t id0= (parent_ino-1u)%fs->sb.inodes_per_group;
        const uint8_t *b0 = fs->bgd_buf + g0 * fs->bgd_size;
        uint64_t it = ((uint64_t)((fs->bgd_size>=64u)?le32(b0+0x28):0u)<<32)|(uint64_t)le32(b0+0x08);
        uint64_t ib = (uint64_t)id0 * fs->sb.inode_size;
        if (ext4_fs_read_block(fs, it + ib/fs->sb.block_size, scratch_inode_block) != 0) {
            say_simple(err, err_len, "parent inode block read (gen)"); return -1;
        }
        parent_gen = le32(scratch_inode_block + (uint32_t)(ib%fs->sb.block_size) + 0x64);
    }

    /* Scan parent dir blocks for entry with file_ino. */
    {
        uint32_t nblks = (uint32_t)((par_inode.size+fs->sb.block_size-1u)/fs->sb.block_size);
        uint32_t b; int found = 0;
        parent_slot_off = 0; parent_prev_off = 0xFFFFFFFFu; parent_dir_block = 0;
        for (b = 0; b < nblks && !found; b++) {
            if (ext4_extent_lookup(fs, par_inode.i_block, b, &dir_phys) != 0) continue;
            parent_dir_block = dir_phys;
            if (ext4_fs_read_block(fs, parent_dir_block, scratch_parent_dir) != 0) continue;
            found = dir_find_entry_by_inode(scratch_parent_dir, fs->sb.block_size, file_ino);
            if (found) { parent_slot_off = s_dir_entry_off; parent_prev_off = s_dir_prev_off; }
        }
        if (!found) { say_simple(err, err_len, "entry not found in parent"); return -1; }
    }

    /* Remove entry: expand predecessor or zero inode field. */
    {
        uint16_t cr = le16(scratch_parent_dir + parent_slot_off + 4);
        if (parent_prev_off != 0xFFFFFFFFu) {
            uint16_t pr = le16(scratch_parent_dir + parent_prev_off + 4);
            uint16_t nr = (uint16_t)(pr + cr);
            scratch_parent_dir[parent_prev_off+4]=(uint8_t)nr;
            scratch_parent_dir[parent_prev_off+5]=(uint8_t)(nr>>8);
        } else {
            scratch_parent_dir[parent_slot_off+0]=scratch_parent_dir[parent_slot_off+1]=0;
            scratch_parent_dir[parent_slot_off+2]=scratch_parent_dir[parent_slot_off+3]=0;
        }
    }
    dir_block_set_tail_csum(fs, scratch_parent_dir, parent_ino, parent_gen);

    /* Free inode: bitmap + BGD (free_inodes++) + SB + zero inode slot. */
    inode_group = (file_ino - 1u) / fs->sb.inodes_per_group;
    inode_bit   = (file_ino - 1u) % fs->sb.inodes_per_group;

    if (ext4_fs_read_block(fs, bgd_fs_block, scratch_bgd) != 0) {
        say_simple(err, err_len, "BGD read failed (final tx)"); return -1;
    }
    {
        uint8_t *be = scratch_bgd + inode_group * fs->bgd_size;
        uint16_t fl; uint32_t fh;
        inode_bmap_phys = (((fs->bgd_size>=64u)?((uint64_t)le32(be+0x24)<<32):0u)|(uint64_t)le32(be+0x04));
        if (ext4_fs_read_block(fs, inode_bmap_phys, scratch_bitmap) != 0) {
            say_simple(err, err_len, "inode bitmap read failed"); return -1;
        }
        scratch_bitmap[inode_bit>>3] &= ~(uint8_t)(1u<<(inode_bit&7u));
        fl=le16(be+0x0E); fh=(fs->bgd_size>=64u)?le16(be+0x2E):0u;
        fl++; if (fl==0u) fh++;
        be[0x0E]=(uint8_t)fl; be[0x0F]=(uint8_t)(fl>>8);
        if (fs->bgd_size>=64u){be[0x2E]=(uint8_t)fh; be[0x2F]=(uint8_t)(fh>>8);}
        bgd_finalize_inode_bitmap_csums(fs, inode_group, be, scratch_bitmap);
    }
    if (ext4_fs_read_block(fs, sb_fs_block, scratch_sb) != 0) {
        say_simple(err, err_len, "SB read failed (final tx)"); return -1;
    }
    {
        uint8_t *sb=scratch_sb+sb_offset_in_block;
        uint32_t fi=le32(sb+0x10); fi++;
        sb[0x10]=(uint8_t)fi; sb[0x11]=(uint8_t)(fi>>8);
        sb[0x12]=(uint8_t)(fi>>16); sb[0x13]=(uint8_t)(fi>>24);
        sb_recompute_csum(fs, sb);
    }
    {
        uint32_t g=inode_group, idx=inode_bit;
        const uint8_t *b0=fs->bgd_buf+g*fs->bgd_size;
        uint64_t it=((uint64_t)((fs->bgd_size>=64u)?le32(b0+0x28):0u)<<32)|(uint64_t)le32(b0+0x08);
        uint64_t ib=(uint64_t)idx*fs->sb.inode_size;
        file_inode_fs_block = it + ib/fs->sb.block_size;
        file_inode_offset   = (uint32_t)(ib%fs->sb.block_size);
    }
    if (ext4_fs_read_block(fs, file_inode_fs_block, scratch_inode_block) != 0) {
        say_simple(err, err_len, "file inode block read failed"); return -1;
    }
    memset(scratch_inode_block + file_inode_offset, 0, fs->sb.inode_size);

    /* Parent inode: update mtime only (file links don't affect parent count). */
    {
        uint32_t g=(parent_ino-1u)/fs->sb.inodes_per_group;
        uint32_t id=(parent_ino-1u)%fs->sb.inodes_per_group;
        const uint8_t *b0=fs->bgd_buf+g*fs->bgd_size;
        uint64_t it=((uint64_t)((fs->bgd_size>=64u)?le32(b0+0x28):0u)<<32)|(uint64_t)le32(b0+0x08);
        uint64_t ib=(uint64_t)id*fs->sb.inode_size;
        parent_inode_fs_block=it+ib/fs->sb.block_size;
        parent_inode_offset=(uint32_t)(ib%fs->sb.block_size);
    }
    if (ext4_fs_read_block(fs, parent_inode_fs_block, scratch_parent_inode) != 0) {
        say_simple(err, err_len, "parent inode block read failed"); return -1;
    }
    {
        uint8_t *pi=scratch_parent_inode+parent_inode_offset;
        inode_set_mtime_and_csum(fs, parent_ino, pi, file_inode.mtime + 1u);
    }

    scratch_trans.block_count = 6u;
    scratch_trans.fs_block[0] = inode_bmap_phys;      scratch_trans.buf[0] = scratch_bitmap;
    scratch_trans.fs_block[1] = bgd_fs_block;          scratch_trans.buf[1] = scratch_bgd;
    scratch_trans.fs_block[2] = sb_fs_block;           scratch_trans.buf[2] = scratch_sb;
    scratch_trans.fs_block[3] = file_inode_fs_block;   scratch_trans.buf[3] = scratch_inode_block;
    scratch_trans.fs_block[4] = parent_dir_block;      scratch_trans.buf[4] = scratch_parent_dir;
    scratch_trans.fs_block[5] = parent_inode_fs_block; scratch_trans.buf[5] = scratch_parent_inode;
    rc = ext4_journal_commit(fs, &scratch_trans, err, err_len);
    if (rc) return -1;

    return 0;
}

/* --- Truncate-down ------------------------------------------------------- */

/* Shrink a regular file to new_size (must be ≤ current size).  Frees data
 * blocks past the new boundary one at a time (3-block tx: bitmap + BGD +
 * fs sb), then commits a final 1-block tx that writes back the inode with
 * the truncated extent tree, updated size, blocks_count, and mtime.
 *
 * The data-block frees happen BEFORE the inode update to avoid a window
 * in which the inode claims fewer blocks than the bitmap reflects (which
 * could let allocator hand out a block we're still in the middle of
 * freeing).  If we crash mid-flight the inode still claims the freed
 * blocks; e2fsck reconciles by re-flagging them as in-use, costing the
 * truncate but never causing aliasing.
 *
 * Refuses extent-tree depth > 0 (same constraint as the rest of the
 * write path).  Same-size or no-op calls return success without writing.
 */
int ext4_file_truncate(struct ext4_fs *fs, uint32_t inode_num,
                       uint64_t new_size, uint32_t now_unix,
                       char *err, uint32_t err_len) {
    static struct ext4_inode file_inode;
    uint32_t       bs;
    uint32_t       new_block_count;
    uint32_t       new_size_lo, new_size_hi;
    uint32_t       blocks_count_lo;
    uint32_t       freed_blocks;
    uint64_t       inode_table_block;
    uint64_t       inode_byte_in_table;
    uint64_t       inode_fs_block;
    uint32_t       inode_offset_in_block;
    uint8_t       *inode_in_block;
    int            rc;

    if (err && err_len) err[0] = '\0';
    if (inode_num == 0u || inode_num == 2u) {
        say_simple(err, err_len, "cannot truncate inode 0 or root"); return -1;
    }
    if (fs->sb.block_size > EXT4_WRITE_BUF_SIZE) {
        say_simple(err, err_len, "block_size > write cap"); return -1;
    }

    if (ext4_inode_read(fs, inode_num, &file_inode) != 0) {
        say_simple(err, err_len, "file inode read failed"); return -1;
    }
    if ((file_inode.mode & 0xF000u) == EXT4_S_IFDIR) {
        say_simple(err, err_len, "is a directory"); return -1;
    }
    if (new_size > file_inode.size) {
        say_simple(err, err_len, "new_size > current size — not a truncate-down"); return -1;
    }
    if (new_size == file_inode.size) return 0;  /* no-op */

    {
        const uint8_t *iblk = file_inode.i_block;
        if (le16(iblk + 0) != EXT4_EXT_MAGIC) {
            say_simple(err, err_len, "file lacks extent header"); return -1;
        }
        if (le16(iblk + 6) != 0) {
            say_simple(err, err_len, "extent tree depth>0"); return -1;
        }
    }

    bs              = fs->sb.block_size;
    new_block_count = (new_size == 0u) ? 0u
                                       : (uint32_t)((new_size + bs - 1u) / bs);
    freed_blocks    = 0u;

    /* ----------------------------------------------------------------
     * Walk extents in reverse: free trailing data blocks (3-block tx
     * each: bitmap + BGD + SB), and rewrite file_inode.i_block in place
     * to drop or shrink extents.  The same modified i_block is then
     * memcpy'd into the inode block in the final transaction.
     * ---------------------------------------------------------------- */
    {
        uint8_t *iblk = file_inode.i_block;
        int      ei;
        uint16_t num_extents = le16(iblk + 2);
        uint16_t kept = num_extents;

        for (ei = (int)num_extents - 1; ei >= 0; ei--) {
            uint8_t *e = iblk + 12u + (uint32_t)ei * 12u;
            uint32_t ext_logical = le32(e + 0);
            uint16_t ext_len     = le16(e + 4);
            uint64_t ext_phys    = ((uint64_t)le16(e + 6) << 32)
                                 | (uint64_t)le32(e + 8);
            uint32_t bi;
            uint32_t free_count_in_extent;

            if (ext_logical >= new_block_count) {
                free_count_in_extent = ext_len;
                kept--;
                memset(e, 0, 12u);
            } else if (ext_logical + ext_len > new_block_count) {
                uint16_t shrunk = (uint16_t)(new_block_count - ext_logical);
                free_count_in_extent = ext_len - shrunk;
                e[4] = (uint8_t) shrunk;
                e[5] = (uint8_t)(shrunk >> 8);
            } else {
                break;  /* extent below boundary; extents are ordered */
            }

            for (bi = 0; bi < free_count_in_extent; bi++) {
                uint64_t blk_phys = ext_phys + (uint64_t)(ext_len - 1u - bi);
                if (free_one_data_block(fs, blk_phys, err, err_len) != 0) return -1;
                freed_blocks++;
            }
        }
        iblk[2] = (uint8_t) kept;
        iblk[3] = (uint8_t)(kept >> 8);
    }

    /* ----------------------------------------------------------------
     * Final transaction: write back inode with truncated extent tree,
     * new size, reduced blocks_count, and bumped mtime.
     * ---------------------------------------------------------------- */
    {
        uint32_t group          = (inode_num - 1u) / fs->sb.inodes_per_group;
        uint32_t index_in_group = (inode_num - 1u) % fs->sb.inodes_per_group;
        const uint8_t *bgd0     = fs->bgd_buf + group * fs->bgd_size;
        uint32_t lo32 = le32(bgd0 + 0x08);
        uint32_t hi32 = (fs->bgd_size >= 64u) ? le32(bgd0 + 0x28) : 0u;
        inode_table_block = ((uint64_t)hi32 << 32) | lo32;
        inode_byte_in_table   = (uint64_t)index_in_group * fs->sb.inode_size;
        inode_fs_block        = inode_table_block + inode_byte_in_table / bs;
        inode_offset_in_block = (uint32_t)(inode_byte_in_table % bs);
    }

    rc = ext4_fs_read_block(fs, inode_fs_block, scratch_inode_block);
    if (rc) { say_simple(err, err_len, "inode block read failed (truncate)"); return -1; }
    inode_in_block = scratch_inode_block + inode_offset_in_block;

    /* Copy the in-memory truncated extent tree into the inode block. */
    memcpy(inode_in_block + 0x28, file_inode.i_block, 60u);

    /* Update inode size. */
    new_size_lo = (uint32_t)(new_size & 0xFFFFFFFFul);
    new_size_hi = (uint32_t)(new_size >> 32);
    inode_in_block[0x04] = (uint8_t) new_size_lo;
    inode_in_block[0x05] = (uint8_t)(new_size_lo >>  8);
    inode_in_block[0x06] = (uint8_t)(new_size_lo >> 16);
    inode_in_block[0x07] = (uint8_t)(new_size_lo >> 24);
    inode_in_block[0x6C] = (uint8_t) new_size_hi;
    inode_in_block[0x6D] = (uint8_t)(new_size_hi >>  8);
    inode_in_block[0x6E] = (uint8_t)(new_size_hi >> 16);
    inode_in_block[0x6F] = (uint8_t)(new_size_hi >> 24);

    /* Reduce inode.blocks_lo (units: 512-byte sectors). */
    blocks_count_lo  = le32(inode_in_block + 0x1C);
    blocks_count_lo -= freed_blocks * (bs / 512u);
    inode_in_block[0x1C] = (uint8_t) blocks_count_lo;
    inode_in_block[0x1D] = (uint8_t)(blocks_count_lo >>  8);
    inode_in_block[0x1E] = (uint8_t)(blocks_count_lo >> 16);
    inode_in_block[0x1F] = (uint8_t)(blocks_count_lo >> 24);

    /* mtime + i_checksum — must be last in-inode write. */
    inode_set_mtime_and_csum(fs, inode_num, inode_in_block, now_unix);

    scratch_trans.block_count = 1u;
    scratch_trans.fs_block[0] = inode_fs_block;
    scratch_trans.buf[0]      = scratch_inode_block;
    rc = ext4_journal_commit(fs, &scratch_trans, err, err_len);
    if (rc) return -1;
    return 0;
}

/* --- In-place directory-entry rename ------------------------------------- */

int ext4_rename(struct ext4_fs *fs, uint32_t parent_ino, uint32_t file_ino,
                const char *new_name, uint8_t new_name_len,
                char *err, uint32_t err_len) {
    static struct ext4_inode par_inode;
    static uint64_t dir_phys;   /* DGROUP — extent_lookup out-pointer */

    uint32_t parent_gen;
    uint64_t parent_dir_block;
    uint32_t entry_off;
    uint16_t entry_rec;
    uint32_t new_real_sz;
    uint64_t parent_inode_fs_block;
    uint32_t parent_inode_offset;
    int rc;

    if (err && err_len) err[0] = '\0';
    if (fs->sb.block_size > EXT4_WRITE_BUF_SIZE) {
        say_simple(err, err_len, "block_size > write cap"); return -1;
    }
    if (new_name_len == 0) {
        say_simple(err, err_len, "new name empty"); return -1;
    }
    new_real_sz = (8u + (uint32_t)new_name_len + 3u) & ~(uint32_t)3u;

    /* Read parent inode to scan its dir blocks. */
    if (ext4_inode_read(fs, parent_ino, &par_inode) != 0) {
        say_simple(err, err_len, "parent inode read failed"); return -1;
    }

    /* Get parent generation for dir-block tail csum. */
    {
        uint32_t g0 = (parent_ino-1u)/fs->sb.inodes_per_group;
        uint32_t id0= (parent_ino-1u)%fs->sb.inodes_per_group;
        const uint8_t *b0 = fs->bgd_buf + g0 * fs->bgd_size;
        uint64_t it = ((uint64_t)((fs->bgd_size>=64u)?le32(b0+0x28):0u)<<32)|(uint64_t)le32(b0+0x08);
        uint64_t ib = (uint64_t)id0 * fs->sb.inode_size;
        if (ext4_fs_read_block(fs, it + ib/fs->sb.block_size, scratch_inode_block) != 0) {
            say_simple(err, err_len, "parent inode block read (gen)"); return -1;
        }
        parent_gen = le32(scratch_inode_block + (uint32_t)(ib%fs->sb.block_size) + 0x64);
    }

    /* Scan parent dir for the entry pointing to file_ino. */
    {
        uint32_t nblks = (uint32_t)((par_inode.size+fs->sb.block_size-1u)/fs->sb.block_size);
        uint32_t b; int found = 0;
        entry_off = 0; entry_rec = 0; parent_dir_block = 0;
        for (b = 0; b < nblks && !found; b++) {
            uint32_t off;
            if (ext4_extent_lookup(fs, par_inode.i_block, b, &dir_phys) != 0) continue;
            parent_dir_block = dir_phys;
            if (ext4_fs_read_block(fs, parent_dir_block, scratch_parent_dir) != 0) continue;
            off = 0;
            while (off + 8u <= fs->sb.block_size) {
                uint16_t rec = le16(scratch_parent_dir + off + 4);
                uint32_t ino = le32(scratch_parent_dir + off);
                if (rec < 8u || rec > fs->sb.block_size - off) break;
                if (scratch_parent_dir[off+7]==0xDEu && rec==12u && ino==0u) {
                    off += rec; continue;
                }
                if (ino == file_ino) {
                    entry_off = off; entry_rec = rec; found = 1; break;
                }
                off += rec;
            }
        }
        if (!found) { say_simple(err, err_len, "entry not found in parent"); return -1; }
    }

    /* Verify new name fits within the existing entry's allocated space. */
    if (new_real_sz > (uint32_t)entry_rec) {
        say_simple(err, err_len, "new name too long for entry slot"); return -1;
    }

    /* Update the entry in scratch_parent_dir: name_len + name bytes.
     * inode, rec_len, and file_type are unchanged. */
    scratch_parent_dir[entry_off + 6] = new_name_len;
    /* Zero old name bytes beyond new length to avoid stale data leaking. */
    {
        uint32_t old_nl = scratch_parent_dir[entry_off + 6]; /* already updated */
        uint32_t clear_start = 8u + new_name_len;
        uint32_t clear_end   = 8u + (uint32_t)(entry_rec - 8u); /* conservative */
        uint32_t k;
        (void)old_nl;
        for (k = clear_start; k < clear_end && k < fs->sb.block_size - entry_off; k++)
            scratch_parent_dir[entry_off + k] = 0;
        memcpy(scratch_parent_dir + entry_off + 8u, new_name, new_name_len);
    }
    dir_block_set_tail_csum(fs, scratch_parent_dir, parent_ino, parent_gen);

    /* Update parent inode mtime. */
    {
        uint32_t g=(parent_ino-1u)/fs->sb.inodes_per_group;
        uint32_t id=(parent_ino-1u)%fs->sb.inodes_per_group;
        const uint8_t *b0=fs->bgd_buf+g*fs->bgd_size;
        uint64_t it=((uint64_t)((fs->bgd_size>=64u)?le32(b0+0x28):0u)<<32)|(uint64_t)le32(b0+0x08);
        uint64_t ib=(uint64_t)id*fs->sb.inode_size;
        parent_inode_fs_block=it+ib/fs->sb.block_size;
        parent_inode_offset=(uint32_t)(ib%fs->sb.block_size);
    }
    if (ext4_fs_read_block(fs, parent_inode_fs_block, scratch_inode_block) != 0) {
        say_simple(err, err_len, "parent inode block read failed"); return -1;
    }
    inode_set_mtime_and_csum(fs, parent_ino,
                             scratch_inode_block + parent_inode_offset,
                             par_inode.mtime + 1u);

    scratch_trans.block_count = 2u;
    scratch_trans.fs_block[0] = parent_dir_block;      scratch_trans.buf[0] = scratch_parent_dir;
    scratch_trans.fs_block[1] = parent_inode_fs_block; scratch_trans.buf[1] = scratch_inode_block;
    rc = ext4_journal_commit(fs, &scratch_trans, err, err_len);
    if (rc) return -1;

    return 0;
}

/* --- Cross-directory rename --------------------------------------------- */

/* Walk new_parent's "..-up" chain looking for moved_ino.
 * Returns 1 if moved_ino is an ancestor (would create a cycle on move),
 * 0 if walk reaches root cleanly, negative on read/format failure. The
 * walk uses scratch_bgd as a one-block scratch (rename's other paths
 * don't touch BGD blocks). Bounded at 64 levels — corrupt fs safety.
 * ".." entry is always at offset 12 (after "." with rec=12); the inode
 * field is the first 4 bytes of the entry.
 *
 * Was host-only under small memory model (cycle detection added ~700
 * bytes that pushed _TEXT over the 64-KiB TSR cap). Medium model has
 * code-segment headroom now, so DOS-side cross-dir directory rename
 * uses the same walker. */
static int xdir_is_ancestor(struct ext4_fs *fs, uint32_t moved_ino,
                            uint32_t descendant_ino) {
    static struct ext4_inode walk_inode;
    static uint64_t          walk_phys;
    uint32_t cur = descendant_ino;
    int      depth;

    for (depth = 0; depth < 64; depth++) {
        if (cur == moved_ino) return 1;
        if (cur == 2u || cur == 0u) return 0;
        if (ext4_inode_read(fs, cur, &walk_inode) != 0) return -1;
        if ((walk_inode.mode & 0xF000u) != EXT4_S_IFDIR) return -1;
        if (ext4_extent_lookup(fs, walk_inode.i_block, 0, &walk_phys) != 0) return -1;
        if (ext4_fs_read_block(fs, walk_phys, scratch_bgd) != 0) return -1;
        cur = le32(scratch_bgd + 12);
    }
    return -1;
}

/* Move file_ino's dir entry from old_parent to new_parent (with new name).
 *
 * For regular files: 4-block transaction (old_dir_block + new_dir_block +
 * both parent inode blocks). When old_parent and new_parent's inodes
 * share an fs_block (consecutive inodes — root's #2 vs a user dir won't
 * collide, but two adjacent user dirs can), the trans collapses to 3
 * blocks with both inodes' edits applied to a single shared buffer.
 *
 * For directories: 5-block transaction adding the moved dir's first
 * data block (with its ".." entry rewritten to point at new_parent),
 * AND old/new parents' i_links_count adjusted (-1/+1) to track the
 * lost/gained ".." backlink. Refuses moving the dir into one of its
 * own descendants (would create a cycle).
 *
 * Atomic: a crash either leaves the file in old_parent or in new_parent —
 * never both, never neither, never with stale ".." or stale link counts.
 *
 * Refuses: htree parents, multi-block parent dirs (same-block dir limit
 * from same-parent rename), and renaming root.
 *
 * Caller guarantees old_parent != new_parent and that the destination
 * name doesn't already exist. */
int ext4_rename_xdir(struct ext4_fs *fs,
                     uint32_t old_parent_ino, uint32_t file_ino,
                     uint32_t new_parent_ino, const char *new_name,
                     uint8_t  new_name_len, uint32_t now_unix,
                     char *err, uint32_t err_len) {
    static struct ext4_inode old_par_inode;
    static struct ext4_inode new_par_inode;
    static struct ext4_inode file_inode;
    static uint64_t old_dir_phys;
    static uint64_t new_dir_phys;
    static uint64_t moved_dir_phys;
    uint32_t       old_par_gen, new_par_gen;
    uint32_t       moved_dir_gen = 0u;
    uint32_t       new_rec_needed;
    uint32_t       slot_off = 0;
    int            found_old = 0, found_new = 0;
    uint32_t       old_slot_off = 0, old_prev_off = 0xFFFFFFFFu;
    uint64_t       old_par_inode_block, new_par_inode_block;
    uint32_t       old_par_inode_offset, new_par_inode_offset;
    uint8_t        ft;
    int            is_dir;
    int            inode_blocks_share;
    uint8_t       *new_par_inode_buf;
    int            rc;

    if (err && err_len) err[0] = '\0';
    if (old_parent_ino == new_parent_ino) {
        say_simple(err, err_len, "same parent — use ext4_rename"); return -1;
    }
    if (new_name_len == 0u) {
        say_simple(err, err_len, "empty name"); return -1;
    }
    if (file_ino == 0u || file_ino == 2u) {
        say_simple(err, err_len, "cannot rename root"); return -1;
    }

    if (ext4_inode_read(fs, old_parent_ino, &old_par_inode) != 0) {
        say_simple(err, err_len, "old parent inode read failed"); return -1;
    }
    if (ext4_inode_read(fs, new_parent_ino, &new_par_inode) != 0) {
        say_simple(err, err_len, "new parent inode read failed"); return -1;
    }
    if (ext4_inode_read(fs, file_ino, &file_inode) != 0) {
        say_simple(err, err_len, "file inode read failed"); return -1;
    }
    if ((old_par_inode.flags & 0x1000u) || (new_par_inode.flags & 0x1000u)) {
        say_simple(err, err_len, "htree parent not supported"); return -1;
    }
    is_dir = ((file_inode.mode & 0xF000u) == EXT4_S_IFDIR);

    if (is_dir) {
        if (file_ino == new_parent_ino) {
            say_simple(err, err_len, "dir into self"); return -1;
        }
        rc = xdir_is_ancestor(fs, file_ino, new_parent_ino);
        if (rc < 0) {
            say_simple(err, err_len, "ancestor walk"); return -1;
        }
        if (rc == 1) {
            say_simple(err, err_len, "dir cycle"); return -1;
        }
    }

    new_rec_needed = (8u + (uint32_t)new_name_len + 3u) & ~(uint32_t)3u;

    /* Old parent inode block. */
    if (read_inode_block(fs, old_parent_ino, scratch_inode_block) != 0) {
        say_simple(err, err_len, "old parent inode block read"); return -1;
    }
    old_par_inode_block  = s_inode_fs_block;
    old_par_inode_offset = s_inode_offset;
    old_par_gen          = s_inode_gen;

    /* New parent inode block. Always read into scratch_parent_inode.
     * Detect whether the two parents share a fs_block; if so, point
     * new_par_inode_buf at scratch_inode_block instead, so dir-rename's
     * nlink ±1 updates land on the same buffer. Otherwise one update
     * would be lost when checkpoint flushes the trans's two journaled
     * copies of the same fs_block. */
    if (read_inode_block(fs, new_parent_ino, scratch_parent_inode) != 0) {
        say_simple(err, err_len, "new parent inode block read"); return -1;
    }
    new_par_inode_block  = s_inode_fs_block;
    new_par_inode_offset = s_inode_offset;
    new_par_inode_buf    = scratch_parent_inode;
    new_par_gen          = s_inode_gen;
    inode_blocks_share   = 0;
    if (new_par_inode_block == old_par_inode_block) {
        inode_blocks_share = 1;
        new_par_inode_buf  = scratch_inode_block;
        new_par_gen        = le32(scratch_inode_block + new_par_inode_offset + 0x64);
    }

    /* Find old entry in old parent's first dir block. */
    if (ext4_extent_lookup(fs, old_par_inode.i_block, 0, &old_dir_phys) != 0) {
        say_simple(err, err_len, "old parent dir block lookup"); return -1;
    }
    if (ext4_fs_read_block(fs, old_dir_phys, scratch_parent_dir) != 0) {
        say_simple(err, err_len, "old parent dir block read"); return -1;
    }
    found_old = dir_find_entry_by_inode(scratch_parent_dir, fs->sb.block_size, file_ino);
    if (!found_old) {
        say_simple(err, err_len, "file not in old parent"); return -1;
    }
    old_slot_off = s_dir_entry_off; old_prev_off = s_dir_prev_off;

    /* Find a slot for the new entry in new parent's first dir block. */
    if (ext4_extent_lookup(fs, new_par_inode.i_block, 0, &new_dir_phys) != 0) {
        say_simple(err, err_len, "new parent dir block lookup"); return -1;
    }
    if (ext4_fs_read_block(fs, new_dir_phys, scratch_data) != 0) {
        say_simple(err, err_len, "new parent dir block read"); return -1;
    }
    found_new = dir_find_slot_for_entry(scratch_data, fs->sb.block_size, new_rec_needed);
    if (!found_new) {
        say_simple(err, err_len, "no room in new parent dir"); return -1;
    }
    slot_off = s_dir_slot_off;

    /* If moving a directory, fetch its i_generation (for dir-block tail
     * csum) and rewrite its first data block's ".." entry to new_parent.
     * Two reads share scratch_bitmap: first the inode block (gen capture),
     * then the data block (the actual edit). */
    if (is_dir) {
        if (ext4_extent_lookup(fs, file_inode.i_block, 0, &moved_dir_phys) != 0) {
            say_simple(err, err_len, "moved dir lookup"); return -1;
        }
        if (read_inode_block(fs, file_ino, scratch_bitmap) != 0) {
            say_simple(err, err_len, "moved dir inode"); return -1;
        }
        moved_dir_gen = s_inode_gen;
        if (ext4_fs_read_block(fs, moved_dir_phys, scratch_bitmap) != 0) {
            say_simple(err, err_len, "moved dir data"); return -1;
        }
        scratch_bitmap[12 + 0] = (uint8_t) new_parent_ino;
        scratch_bitmap[12 + 1] = (uint8_t)(new_parent_ino >>  8);
        scratch_bitmap[12 + 2] = (uint8_t)(new_parent_ino >> 16);
        scratch_bitmap[12 + 3] = (uint8_t)(new_parent_ino >> 24);
        dir_block_set_tail_csum(fs, scratch_bitmap, file_ino, moved_dir_gen);
    }

    /* Insert new entry in new parent. file_type carried over from file. */
    if (fs->sb.feature_incompat & 0x2u) {
        ft = is_dir ? 2u : 1u;  /* EXT4_FT_DIR : EXT4_FT_REGULAR */
    } else {
        ft = 0u;
    }
    {
        uint32_t remaining = s_dir_slot_rec;
        scratch_data[slot_off + 0] = (uint8_t) file_ino;
        scratch_data[slot_off + 1] = (uint8_t)(file_ino >>  8);
        scratch_data[slot_off + 2] = (uint8_t)(file_ino >> 16);
        scratch_data[slot_off + 3] = (uint8_t)(file_ino >> 24);
        scratch_data[slot_off + 4] = (uint8_t) remaining;
        scratch_data[slot_off + 5] = (uint8_t)(remaining >> 8);
        scratch_data[slot_off + 6] = new_name_len;
        scratch_data[slot_off + 7] = ft;
        memcpy(scratch_data + slot_off + 8u, new_name, new_name_len);
    }
    dir_block_set_tail_csum(fs, scratch_data, new_parent_ino, new_par_gen);

    /* Remove old entry: extend predecessor's rec_len, or zero inode field. */
    {
        uint16_t cr = le16(scratch_parent_dir + old_slot_off + 4);
        if (old_prev_off != 0xFFFFFFFFu) {
            uint16_t pr = le16(scratch_parent_dir + old_prev_off + 4);
            uint16_t nr = (uint16_t)(pr + cr);
            scratch_parent_dir[old_prev_off + 4] = (uint8_t) nr;
            scratch_parent_dir[old_prev_off + 5] = (uint8_t)(nr >> 8);
        } else {
            scratch_parent_dir[old_slot_off + 0] = 0;
            scratch_parent_dir[old_slot_off + 1] = 0;
            scratch_parent_dir[old_slot_off + 2] = 0;
            scratch_parent_dir[old_slot_off + 3] = 0;
        }
    }
    dir_block_set_tail_csum(fs, scratch_parent_dir, old_parent_ino, old_par_gen);

    /* Update parent inode bytes: nlink ±1 for dir-rename, mtime, csum.
     * Do nlink BEFORE csum so the csum covers the final state. */
    if (is_dir) {
        uint8_t  *pi = scratch_inode_block + old_par_inode_offset;
        uint16_t  lc = le16(pi + 0x1A);
        if (lc > 0u) lc--;
        pi[0x1A] = (uint8_t)lc; pi[0x1B] = (uint8_t)(lc >> 8);
    }
    inode_set_mtime_and_csum(fs, old_parent_ino,
                             scratch_inode_block + old_par_inode_offset, now_unix);
    if (is_dir) {
        uint8_t  *pi = new_par_inode_buf + new_par_inode_offset;
        uint16_t  lc = le16(pi + 0x1A);
        lc++;
        pi[0x1A] = (uint8_t)lc; pi[0x1B] = (uint8_t)(lc >> 8);
    }
    inode_set_mtime_and_csum(fs, new_parent_ino,
                             new_par_inode_buf + new_par_inode_offset, now_unix);

    /* Build trans. Slot count varies:
     *   regular file, separate inode blocks: 4 slots
     *   regular file, shared inode block:    3 slots
     *   directory,    separate inode blocks: 5 slots (+moved-dir block)
     *   directory,    shared inode block:    4 slots */
    {
        uint32_t k = 0;
        scratch_trans.fs_block[k] = old_dir_phys;        scratch_trans.buf[k++] = scratch_parent_dir;
        scratch_trans.fs_block[k] = new_dir_phys;        scratch_trans.buf[k++] = scratch_data;
        scratch_trans.fs_block[k] = old_par_inode_block; scratch_trans.buf[k++] = scratch_inode_block;
        if (!inode_blocks_share) {
            scratch_trans.fs_block[k] = new_par_inode_block;
            scratch_trans.buf[k++]    = scratch_parent_inode;
        }
        if (is_dir) {
            scratch_trans.fs_block[k] = moved_dir_phys;
            scratch_trans.buf[k++]    = scratch_bitmap;
        }
        scratch_trans.block_count = k;
    }
    rc = ext4_journal_commit(fs, &scratch_trans, err, err_len);
    if (rc) return -1;
    return 0;
}

#ifndef __WATCOMC__
/* Host-only convenience helper used by host_write_test / host_stress_test.
 * Excluded from the DOS TSR to keep _TEXT under 64 KiB — the TSR reads
 * blocks one at a time via ext4_file_read_block, doesn't need the
 * gather-multiple-blocks variant. */
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
#endif
