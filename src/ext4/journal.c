#include "journal.h"
#include "fs.h"
#include "inode.h"
#include "extent.h"
#include "../blockdev/blockdev.h"
#include "../util/endian.h"
#include "../util/crc32c.h"
#include <stdio.h>
#include <string.h>

/* Phase 0 / B: parse jbd2 superblock and walk the log to build a
 * {fs_block -> journal_blk} replay map. Soft replay only — no writes. */

/* Walk actions — three passes, lwext4-style. SCAN finds the last
 * contiguous valid commit; REVOKE populates the revoke map; BUILD
 * populates the replay map (consulting the now-complete revoke map). */
#define JBD_ACTION_SCAN   0
#define JBD_ACTION_REVOKE 1
#define JBD_ACTION_BUILD  2

/* DGROUP statics — DOS small-model has ~4 KB stack, and bdev_read
 * paths can run with SS != DS in interrupt context. See feedback memory:
 * dos_stack_size. Both buffers are reused across calls within a single
 * mount; init runs first (uses jsb_buf), then replay walks (uses
 * walk_buf), then both are dormant. */
static uint8_t jsb_buf[4096];
static uint8_t walk_buf[4096];

static void say(char *err, size_t err_len, const char *msg) {
    if (err && err_len) {
        size_t n = strlen(msg);
        if (n >= err_len) n = err_len - 1u;
        memcpy(err, msg, n);
        err[n] = '\0';
    }
}

/* Sequence numbers wrap at 2^32. Compare with signed-difference. */
static int seq_diff(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

/* Big-endian writers — jbd2 wire format. */
static void be32_pack(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t) v;
}
static void be16_pack(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t) v;
}

/* Translate a journal-internal block index (0..maxlen-1) to an absolute
 * fs block via the journal inode's extent table, then read it. */
int ext4_journal_read_log_block(struct ext4_fs *fs, uint32_t jblk, void *out_buf) {
    uint32_t i;
    uint64_t phys;
    uint32_t byte, sector, sectors;

    for (i = 0; i < fs->jbd.extent_count; i++) {
        const struct ext4_jbd_extent *e = &fs->jbd.extents[i];
        if (jblk >= e->logical && jblk < e->logical + e->length) {
            phys = e->physical + (uint64_t)(jblk - e->logical);
            byte    = (uint32_t)(phys * (uint64_t)fs->sb.block_size);
            sector  = (uint32_t)(fs->partition_lba + byte / fs->bd->sector_size);
            sectors = fs->sb.block_size / fs->bd->sector_size;
            return bdev_read(fs->bd, sector, sectors, out_buf);
        }
    }
    return -1;
}

static void wrap_jblk(const struct ext4_fs *fs, uint32_t *jblk) {
    if (*jblk >= fs->jbd.maxlen) *jblk = fs->jbd.first;
}

/* Map upserters. Linear search is fine — caps are small (256/64) and
 * this only runs at mount time. */
static int replay_upsert(struct ext4_jbd *j, uint64_t fs_block,
                         uint32_t journal_blk, uint8_t is_escape) {
    uint32_t i;
    for (i = 0; i < j->replay_count; i++) {
        if (j->replay[i].fs_block == fs_block) {
            j->replay[i].journal_blk = journal_blk;
            j->replay[i].is_escape   = is_escape;
            return 0;
        }
    }
    if (j->replay_count >= EXT4_JBD_REPLAY_MAP_MAX) return -1;
    j->replay[j->replay_count].fs_block    = fs_block;
    j->replay[j->replay_count].journal_blk = journal_blk;
    j->replay[j->replay_count].is_escape   = is_escape;
    j->replay_count++;
    return 0;
}

static int revoke_upsert(struct ext4_jbd *j, uint64_t fs_block,
                         uint32_t revoke_seq) {
    uint32_t i;
    for (i = 0; i < j->revoke_count; i++) {
        if (j->revoke[i].fs_block == fs_block) {
            /* Keep the highest seq — later revokes shadow earlier. */
            if (seq_diff(revoke_seq, j->revoke[i].revoke_seq) > 0)
                j->revoke[i].revoke_seq = revoke_seq;
            return 0;
        }
    }
    if (j->revoke_count >= EXT4_JBD_REVOKE_MAP_MAX) return -1;
    j->revoke[j->revoke_count].fs_block   = fs_block;
    j->revoke[j->revoke_count].revoke_seq = revoke_seq;
    j->revoke_count++;
    return 0;
}

/* A revoke at seq R means: skip tags from transactions whose seq <= R. */
static int is_revoked(const struct ext4_jbd *j, uint64_t fs_block, uint32_t this_seq) {
    uint32_t i;
    for (i = 0; i < j->revoke_count; i++) {
        if (j->revoke[i].fs_block == fs_block) {
            return seq_diff(j->revoke[i].revoke_seq, this_seq) >= 0;
        }
    }
    return 0;
}

/* Verify a descriptor or revoke block's tail checksum. Only meaningful
 * when CSUM_V2 or CSUM_V3 is enabled — V1 (or no-csum) has no tail. The
 * tail is the last 4 bytes of the block, holding crc32c(uuid + block)
 * computed with the tail itself zeroed at write time. */
static int verify_meta_block(const struct ext4_jbd *j, uint8_t *buf) {
    uint32_t orig, computed;
    uint8_t  saved[4];

    if (!(j->csum_v2 || j->csum_v3)) return 1;
    memcpy(saved, buf + j->blocksize - 4u, 4);
    orig = be32(saved);
    memset(buf + j->blocksize - 4u, 0, 4);
    computed = crc32c(CRC32C_INIT, j->uuid, 16);
    computed = crc32c(computed,    buf, j->blocksize);
    memcpy(buf + j->blocksize - 4u, saved, 4);
    return computed == orig;
}

/* Verify a commit block's checksum. chksum[0] sits at offset 16
 * (4 bytes), holding crc32c(uuid + commit_block_with_chksum0_zeroed). */
static int verify_commit_block(const struct ext4_jbd *j, uint8_t *buf) {
    uint32_t orig, computed;
    uint8_t  saved[4];

    if (!(j->csum_v2 || j->csum_v3)) return 1;
    memcpy(saved, buf + 16u, 4);
    orig = be32(saved);
    memset(buf + 16u, 0, 4);
    computed = crc32c(CRC32C_INIT, j->uuid, 16);
    computed = crc32c(computed,    buf, j->blocksize);
    memcpy(buf + 16u, saved, 4);
    return computed == orig;
}

/* Per-FS tag size, sans UUID. Mirrors lwext4 jbd_tag_bytes. */
static uint32_t jbd_tag_bytes(const struct ext4_jbd *j) {
    if (j->csum_v3) return 16;          /* tag3: full crc32c, always 64-bit-capable */
    {
        uint32_t s = 12;                /* tag (blocknr + csum + flags + blocknr_high) */
        if (j->csum_v2) s += 0;         /* csum already in tag */
        if (!j->has_64bit) s -= 4;      /* drop blocknr_high */
        return s;
    }
}

/* Walk a descriptor block's tag table. For each tag we advance
 * *this_block to point at that tag's data block (immediately following
 * the descriptor in the log). On BUILD, we add to the replay map.
 * Other actions just walk to keep this_block in sync. */
static int iterate_descriptor_tags(struct ext4_fs *fs, uint32_t this_seq,
                                   uint32_t *this_block, const uint8_t *buf,
                                   int action) {
    uint32_t tag_bytes = jbd_tag_bytes(&fs->jbd);
    int32_t  remaining = (int32_t)fs->jbd.blocksize - 12; /* sub jbd_bhdr */
    const uint8_t *p   = buf + 12;
    int last_tag = 0;

    /* Both v2 and v3 reserve 4 bytes at the descriptor tail for crc. */
    if (fs->jbd.csum_v2 || fs->jbd.csum_v3) remaining -= 4;

    while (!last_tag && remaining >= (int32_t)tag_bytes) {
        uint64_t fs_block;
        uint32_t flags;
        int      is_escape, same_uuid;

        if (fs->jbd.csum_v3) {
            /* tag3 layout: blocknr(4), flags(4), blocknr_high(4), checksum(4) */
            fs_block = (uint64_t)be32(p + 0);
            flags    = be32(p + 4);
            if (fs->jbd.has_64bit) fs_block |= ((uint64_t)be32(p + 8) << 32);
        } else {
            /* tag layout: blocknr(4), checksum(2), flags(2), blocknr_high(4 if 64bit) */
            fs_block = (uint64_t)be32(p + 0);
            flags    = (uint32_t)be16(p + 6);
            if (fs->jbd.has_64bit) fs_block |= ((uint64_t)be32(p + 8) << 32);
        }

        is_escape = (flags & EXT4_JBD_TAG_FLAG_ESCAPE)    ? 1 : 0;
        same_uuid = (flags & EXT4_JBD_TAG_FLAG_SAME_UUID) ? 1 : 0;
        last_tag  = (flags & EXT4_JBD_TAG_FLAG_LAST_TAG)  ? 1 : 0;

        /* Advance past the descriptor (or the previous tag's data block)
         * to point at THIS tag's data block. */
        (*this_block)++;
        wrap_jblk(fs, this_block);

        if (action == JBD_ACTION_BUILD) {
            if (!is_revoked(&fs->jbd, fs_block, this_seq)) {
                if (replay_upsert(&fs->jbd, fs_block, *this_block,
                                  (uint8_t)is_escape) < 0) {
                    return -1; /* cap exceeded */
                }
            }
        }

        p         += tag_bytes;
        remaining -= (int32_t)tag_bytes;
        if (!same_uuid) {
            /* The first tag of a descriptor (and any after a non-SAME_UUID)
             * carries 16 UUID bytes immediately after the tag struct. */
            if (remaining < 16) break;
            p         += 16;
            remaining -= 16;
        }
    }
    return 0;
}

/* Parse a revoke block, adding each revoked fs_block to the revoke map
 * with seq = this_seq. */
static int handle_revoke(struct ext4_jbd *j, uint32_t this_seq, const uint8_t *buf) {
    /* jbd_revoke_header: bhdr(12) + count(4) = 16 */
    uint32_t count_bytes = be32(buf + 12);
    uint32_t record_len  = j->has_64bit ? 8u : 4u;
    uint32_t header_size = 16u;
    uint32_t nr_entries;
    uint32_t i;
    uint32_t max_in_block;

    /* Tail crc on v2/v3 — count_bytes already excludes it on disk, but
     * cap at block size to be safe. */
    max_in_block = (j->blocksize - header_size) / record_len;
    if (count_bytes < header_size) return 0;
    nr_entries = (count_bytes - header_size) / record_len;
    if (nr_entries > max_in_block) nr_entries = max_in_block;

    for (i = 0; i < nr_entries; i++) {
        const uint8_t *p = buf + header_size + i * record_len;
        uint64_t fs_block = (record_len == 8u) ? be64(p) : (uint64_t)be32(p);
        if (revoke_upsert(j, fs_block, this_seq) < 0) return -1;
    }
    return 0;
}

/* The core walker. Three-pass model: caller invokes with SCAN first to
 * find last_seq, then REVOKE, then BUILD. */
static int walk_log(struct ext4_fs *fs, int action, uint32_t *last_seq_io) {
    uint32_t this_block  = fs->jbd.start;
    uint32_t this_seq    = fs->jbd.sequence;
    uint32_t start_block = this_block;
    uint32_t end_seq     = (action == JBD_ACTION_SCAN) ? 0 : *last_seq_io;
    int      log_end     = 0;
    int      iter_guard  = 0;     /* defensive: bound total blocks visited */
    int      rc;

    if (fs->jbd.start == 0) return 0; /* clean log */

    while (!log_end) {
        uint32_t magic, blocktype, seq;

        /* For non-SCAN passes, stop once we've processed past last_seq. */
        if (action != JBD_ACTION_SCAN) {
            if (seq_diff(this_seq, end_seq) > 0) { log_end = 1; continue; }
        }

        rc = ext4_journal_read_log_block(fs, this_block, walk_buf);
        if (rc) return -2;

        magic     = be32(walk_buf + 0);
        blocktype = be32(walk_buf + 4);
        seq       = be32(walk_buf + 8);

        if (magic != EXT4_JBD_MAGIC) { log_end = 1; continue; }
        if (seq != this_seq) {
            /* Sequence mismatch = end of valid log (uncomitted-tail data
             * still in the circular buffer from a prior epoch). On non-
             * SCAN this signals corruption — bail. */
            if (action != JBD_ACTION_SCAN) return -3;
            log_end = 1;
            continue;
        }

        switch (blocktype) {
        case EXT4_JBD_BT_DESCRIPTOR:
            /* CSUM_V2/V3: bad descriptor tail = treat as end of log. The
             * commit for THIS transaction never landed atomically. */
            if (!verify_meta_block(&fs->jbd, walk_buf)) { log_end = 1; break; }
            rc = iterate_descriptor_tags(fs, this_seq, &this_block, walk_buf, action);
            if (rc) return -4;
            break;

        case EXT4_JBD_BT_COMMIT:
            if (!verify_commit_block(&fs->jbd, walk_buf)) { log_end = 1; break; }
            this_seq++;
            if (action == JBD_ACTION_SCAN) end_seq = this_seq;
            break;

        case EXT4_JBD_BT_REVOKE:
            if (!verify_meta_block(&fs->jbd, walk_buf)) { log_end = 1; break; }
            if (action == JBD_ACTION_REVOKE) {
                rc = handle_revoke(&fs->jbd, this_seq, walk_buf);
                if (rc) return -5;
            }
            break;

        default:
            log_end = 1;
            break;
        }

        this_block++;
        wrap_jblk(fs, &this_block);
        if (this_block == start_block) log_end = 1;

        /* Shouldn't be reachable for any sane log — guard against an
         * infinite loop from a corrupt sequence/start configuration. */
        if (++iter_guard > (int)(fs->jbd.maxlen + 4u)) return -6;
    }

    if (action == JBD_ACTION_SCAN) {
        /* end_seq was incremented past the last valid commit. If at least
         * one commit was processed, the last valid trans is end_seq - 1. */
        if (seq_diff(end_seq, fs->jbd.sequence) > 0)
            *last_seq_io = end_seq - 1;
        else
            *last_seq_io = end_seq;
    }
    return 0;
}

int ext4_journal_replay(struct ext4_fs *fs, char *err, uint32_t err_len) {
    uint32_t last_seq;
    int      rc;

    if (err && err_len) err[0] = '\0';
    if (!fs->jbd.present)  return 0;
    if (fs->jbd.start == 0) return 0; /* clean log */

    /* Pass 1: SCAN — find last contiguous valid commit. */
    last_seq = 0;
    rc = walk_log(fs, JBD_ACTION_SCAN, &last_seq);
    if (rc) {
        snprintf(err, err_len, "journal scan failed (rc=%d)", rc);
        return rc;
    }
    /* No valid transactions — nothing to replay. */
    if (seq_diff(last_seq, fs->jbd.sequence) < 0) return 0;

    /* Pass 2: REVOKE — populate revoke map. */
    rc = walk_log(fs, JBD_ACTION_REVOKE, &last_seq);
    if (rc) {
        fs->jbd.revoke_count = 0;
        snprintf(err, err_len, "journal revoke pass failed (rc=%d)", rc);
        return rc;
    }

    /* Pass 3: BUILD — populate replay map, skipping tags shadowed by
     * the revoke map. */
    rc = walk_log(fs, JBD_ACTION_BUILD, &last_seq);
    if (rc) {
        fs->jbd.replay_count = 0;
        fs->jbd.revoke_count = 0;
        snprintf(err, err_len, "journal build pass failed (rc=%d)", rc);
        return rc;
    }

    fs->jbd.replay_active = (uint8_t)((fs->jbd.replay_count > 0) ? 1 : 0);
    return 0;
}

int ext4_journal_lookup(const struct ext4_fs *fs, uint64_t fs_block,
                        uint32_t *out_jblk, uint8_t *out_is_escape) {
    uint32_t i;
    if (!fs->jbd.replay_active) return 0;
    for (i = 0; i < fs->jbd.replay_count; i++) {
        if (fs->jbd.replay[i].fs_block == fs_block) {
            if (out_jblk)      *out_jblk      = fs->jbd.replay[i].journal_blk;
            if (out_is_escape) *out_is_escape = fs->jbd.replay[i].is_escape;
            return 1;
        }
    }
    return 0;
}

/* --- Phase 1a: hard checkpoint (write replay map back to disk) ----------- */

/* Write one journal-recorded block back to its on-disk fs_block
 * location. Restores the JBD magic if the tag was escaped. Reuses
 * walk_buf — phase B's walker isn't running concurrently with this. */
static int checkpoint_one_block(struct ext4_fs *fs, uint64_t fs_block,
                                uint32_t jblk, uint8_t is_escape) {
    uint32_t byte, sector, sectors;
    int      rc;

    rc = ext4_journal_read_log_block(fs, jblk, walk_buf);
    if (rc) return rc;
    if (is_escape) {
        walk_buf[0] = 0xC0; walk_buf[1] = 0x3B;
        walk_buf[2] = 0x39; walk_buf[3] = 0x98;
    }
    byte    = (uint32_t)(fs_block * (uint64_t)fs->sb.block_size);
    sector  = (uint32_t)(fs->partition_lba + byte / fs->bd->sector_size);
    sectors = fs->sb.block_size / fs->bd->sector_size;
    return bdev_write(fs->bd, sector, sectors, walk_buf);
}

/* Write one block to the journal log via the journal-inode extent
 * table. Symmetric to ext4_journal_read_log_block on the read side. */
static int write_journal_block(struct ext4_fs *fs, uint32_t jblk, const void *buf) {
    uint32_t i;
    uint64_t phys;
    uint32_t byte, sector, sectors;

    for (i = 0; i < fs->jbd.extent_count; i++) {
        const struct ext4_jbd_extent *e = &fs->jbd.extents[i];
        if (jblk >= e->logical && jblk < e->logical + e->length) {
            phys    = e->physical + (uint64_t)(jblk - e->logical);
            byte    = (uint32_t)(phys * (uint64_t)fs->sb.block_size);
            sector  = (uint32_t)(fs->partition_lba + byte / fs->bd->sector_size);
            sectors = fs->sb.block_size / fs->bd->sector_size;
            return bdev_write(fs->bd, sector, sectors, buf);
        }
    }
    return -1;
}

/* Re-read the journal superblock, write s_start and s_sequence,
 * recompute jsb checksum if CSUM_V2/V3 is enabled. */
static int update_jsb(struct ext4_fs *fs, uint32_t new_start, uint32_t new_seq) {
    uint64_t byte;
    uint32_t sector, sectors;
    uint64_t first_phys;
    int      rc;

    first_phys = fs->jbd.extents[0].physical;
    byte    = first_phys * (uint64_t)fs->sb.block_size;
    sector  = (uint32_t)(fs->partition_lba + byte / fs->bd->sector_size);
    sectors = fs->sb.block_size / fs->bd->sector_size;

    rc = bdev_read(fs->bd, sector, sectors, jsb_buf);
    if (rc) return rc;

    /* s_sequence at 0x18, s_start at 0x1C (both big-endian). */
    be32_pack(jsb_buf + 0x18, new_seq);
    be32_pack(jsb_buf + 0x1C, new_start);

    if (fs->jbd.csum_v2 || fs->jbd.csum_v3) {
        uint32_t csum;
        jsb_buf[0xFC] = jsb_buf[0xFD] = 0;
        jsb_buf[0xFE] = jsb_buf[0xFF] = 0;
        /* Whole SB, post-zeroing. Stored big-endian. */
        csum = crc32c(CRC32C_INIT, jsb_buf, fs->sb.block_size);
        be32_pack(jsb_buf + 0xFC, csum);
    }

    return bdev_write(fs->bd, sector, sectors, jsb_buf);
}

/* Re-read the FS superblock, set or clear EXT4_FEATURE_INCOMPAT_RECOVER,
 * recompute fs sb checksum if metadata_csum is enabled. */
static int update_fs_sb_recover(struct ext4_fs *fs, int set) {
    uint32_t feat_incompat;
    uint32_t sector, sectors;
    /* The fs SB is exactly 1024 bytes at byte offset 1024 of the
     * partition. Reuse jsb_buf — DGROUP is precious in DOS small-model
     * and jsb_buf is dormant by the time we touch the fs SB. */
    uint8_t *sb = jsb_buf;
    int      rc;

    sector  = (uint32_t)(fs->partition_lba + 1024u / fs->bd->sector_size);
    sectors = 1024u / fs->bd->sector_size;

    rc = bdev_read(fs->bd, sector, sectors, sb);
    if (rc) return rc;

    feat_incompat = le32(sb + 0x60);
    if (set) feat_incompat |=  (uint32_t)0x4u;
    else     feat_incompat &= ~(uint32_t)0x4u;
    sb[0x60] = (uint8_t) feat_incompat;
    sb[0x61] = (uint8_t)(feat_incompat >>  8);
    sb[0x62] = (uint8_t)(feat_incompat >> 16);
    sb[0x63] = (uint8_t)(feat_incompat >> 24);

    /* metadata_csum is at feature_ro_compat bit 0x400 (offset 0x64). */
    if (le32(sb + 0x64) & 0x400u) {
        uint32_t csum;
        sb[0x3FC] = sb[0x3FD] = sb[0x3FE] = sb[0x3FF] = 0;
        /* Spec: crc32c covers bytes 0..0x3FB ONLY (size = offsetof(checksum)).
         * NOT the same convention as jsb (which CRCs the whole 1 KiB
         * with the chksum field zeroed). */
        csum = crc32c(CRC32C_INIT, sb, 0x3FC);
        sb[0x3FC] = (uint8_t) csum;
        sb[0x3FD] = (uint8_t)(csum >>  8);
        sb[0x3FE] = (uint8_t)(csum >> 16);
        sb[0x3FF] = (uint8_t)(csum >> 24);
    }

    return bdev_write(fs->bd, sector, sectors, sb);
}

/* --- Phase 1b: emit a transaction (descriptor + data + commit) -------- */

int ext4_journal_commit(struct ext4_fs *fs, struct ext4_jbd_trans *trans,
                        char *err, uint32_t err_len) {
    /* walk_buf is the scratch for descriptor and commit blocks (used
     * sequentially — descriptor is durable before we overwrite the
     * scratch with the commit block). */
    uint32_t this_seq, this_first;
    uint32_t i;
    uint32_t tag_bytes;
    uint8_t  seq_be[4];
    int      rc;

    if (err && err_len) err[0] = '\0';
    if (!fs->jbd.present) {
        say(err, err_len, "no journal");
        return -1;
    }
    if (!bdev_writable(fs->bd)) {
        say(err, err_len, "bdev read-only");
        return -2;
    }
    if (fs->jbd.start != 0) {
        say(err, err_len, "journal not clean — checkpoint dirty journal first");
        return -3;
    }
    if (trans->block_count == 0 || trans->block_count > EXT4_JBD_TRANS_MAX_BLOCKS) {
        snprintf(err, err_len, "trans block_count %lu out of range",
                 (unsigned long)trans->block_count);
        return -4;
    }
    if (fs->sb.feature_ro_compat & 0x400u) {
        say(err, err_len, "metadata_csum write support is phase 1c");
        return -5;
    }

    /* Reject blocks whose first u32 matches JBD_MAGIC. ESCAPE handling
     * (zero the first u32 in the journal copy + set ESCAPE flag + restore
     * on read) is phase 1c; for in-place file writes this is rare. */
    for (i = 0; i < trans->block_count; i++) {
        const uint8_t *p = (const uint8_t *)trans->buf[i];
        if (be32(p) == EXT4_JBD_MAGIC) {
            say(err, err_len, "block first-u32 is JBD_MAGIC; ESCAPE not yet supported");
            return -6;
        }
    }

    this_seq   = fs->jbd.sequence;
    this_first = fs->jbd.first;
    tag_bytes  = jbd_tag_bytes(&fs->jbd);

    /* Descriptor + N data + commit must fit in the log starting at
     * jsb.first. (Phase 1 doesn't wrap the log.) */
    if ((uint32_t)(this_first + 1u + trans->block_count + 1u) > fs->jbd.maxlen) {
        say(err, err_len, "journal log too small for trans");
        return -7;
    }

    /* Tag table must fit in one descriptor block. Capacity: blocksize
     * minus bhdr (12) minus tail csum (4 if csum_v2/v3) minus first-tag
     * UUID (16 bytes after the first tag). */
    {
        int32_t cap = (int32_t)fs->jbd.blocksize - 12 - 16;
        if (fs->jbd.csum_v2 || fs->jbd.csum_v3) cap -= 4;
        if ((int32_t)(trans->block_count * tag_bytes) > cap) {
            say(err, err_len, "too many blocks for one descriptor");
            return -8;
        }
    }

    /* --- Build descriptor block in walk_buf --- */
    memset(walk_buf, 0, fs->jbd.blocksize);
    be32_pack(walk_buf + 0, EXT4_JBD_MAGIC);
    be32_pack(walk_buf + 4, EXT4_JBD_BT_DESCRIPTOR);
    be32_pack(walk_buf + 8, this_seq);

    be32_pack(seq_be, this_seq);
    {
        uint32_t tag_off = 12;
        for (i = 0; i < trans->block_count; i++) {
            uint32_t flags = 0;
            uint64_t fs_blk = trans->fs_block[i];

            if (i + 1u == trans->block_count) flags |= EXT4_JBD_TAG_FLAG_LAST_TAG;
            if (i > 0)                        flags |= EXT4_JBD_TAG_FLAG_SAME_UUID;

            if (fs->jbd.csum_v3) {
                /* tag3: blocknr(4) | flags(4) | blocknr_high(4) | csum(4) */
                be32_pack(walk_buf + tag_off + 0, (uint32_t)fs_blk);
                be32_pack(walk_buf + tag_off + 4, flags);
                be32_pack(walk_buf + tag_off + 8, (uint32_t)(fs_blk >> 32));
                /* csum filled below */
            } else {
                /* tag: blocknr(4) | csum16(2) | flags16(2) | blocknr_high(4 if 64bit) */
                be32_pack(walk_buf + tag_off + 0, (uint32_t)fs_blk);
                /* csum16 at +4..+5, filled below */
                be16_pack(walk_buf + tag_off + 6, (uint16_t)flags);
                if (fs->jbd.has_64bit) {
                    be32_pack(walk_buf + tag_off + 8, (uint32_t)(fs_blk >> 32));
                }
            }

            /* Per-tag csum: crc32c(uuid + seq_be + data_block).
             * V3 stores the full 32-bit value in the tag's csum field;
             * V2 truncates to the low 16 bits. */
            if (fs->jbd.csum_v2 || fs->jbd.csum_v3) {
                uint32_t per = crc32c(CRC32C_INIT, fs->jbd.uuid, 16);
                per = crc32c(per, seq_be, 4);
                per = crc32c(per, trans->buf[i], fs->jbd.blocksize);
                if (fs->jbd.csum_v3) {
                    be32_pack(walk_buf + tag_off + 12, per);
                } else {
                    be16_pack(walk_buf + tag_off + 4, (uint16_t)(per & 0xFFFFu));
                }
            }

            tag_off += tag_bytes;
            /* First tag carries the UUID immediately after; later tags
             * use SAME_UUID. */
            if (i == 0) {
                memcpy(walk_buf + tag_off, fs->jbd.uuid, 16);
                tag_off += 16;
            }
        }
    }

    /* Descriptor tail crc (CSUM_V2/V3 only). Tail field is the last 4
     * bytes; CRC is over uuid + entire descriptor with tail zeroed. */
    if (fs->jbd.csum_v2 || fs->jbd.csum_v3) {
        uint32_t tail = crc32c(CRC32C_INIT, fs->jbd.uuid, 16);
        tail = crc32c(tail, walk_buf, fs->jbd.blocksize);
        be32_pack(walk_buf + fs->jbd.blocksize - 4u, tail);
    }

    /* Step 1: SET RECOVER on FS SB. After this, if we crash, next mount
     * tries replay; finds either a valid commit (replays) or invalid
     * (clears RECOVER on next clean unmount). */
    rc = update_fs_sb_recover(fs, 1);
    if (rc) { say(err, err_len, "set RECOVER failed"); return rc; }

    /* Step 2: write descriptor at journal[first]. */
    rc = write_journal_block(fs, this_first, walk_buf);
    if (rc) { say(err, err_len, "descriptor write failed"); return rc; }

    /* Step 3: write data blocks at journal[first+1..first+N]. */
    for (i = 0; i < trans->block_count; i++) {
        rc = write_journal_block(fs, this_first + 1u + i, trans->buf[i]);
        if (rc) {
            snprintf(err, err_len, "data write failed at #%lu (rc=%d)",
                     (unsigned long)i, rc);
            return rc;
        }
    }

    /* --- Build commit block in walk_buf (overwrites descriptor in scratch) --- */
    memset(walk_buf, 0, fs->jbd.blocksize);
    be32_pack(walk_buf + 0, EXT4_JBD_MAGIC);
    be32_pack(walk_buf + 4, EXT4_JBD_BT_COMMIT);
    be32_pack(walk_buf + 8, this_seq);
    if (fs->jbd.csum_v2 || fs->jbd.csum_v3) {
        uint32_t commit_csum;
        walk_buf[12] = 4u; /* chksum_type = JBD_CRC32C_CHKSUM */
        walk_buf[13] = 4u; /* chksum_size */
        /* chksum[0] at offset 16, currently zero. CRC over uuid + commit block. */
        commit_csum = crc32c(CRC32C_INIT, fs->jbd.uuid, 16);
        commit_csum = crc32c(commit_csum, walk_buf, fs->jbd.blocksize);
        be32_pack(walk_buf + 16, commit_csum);
    }

    /* Step 4: write commit block at journal[first+1+N]. */
    rc = write_journal_block(fs, this_first + 1u + trans->block_count, walk_buf);
    if (rc) { say(err, err_len, "commit block write failed"); return rc; }

    /* Step 5: update jsb on disk so a future mount sees our trans.
     * Atomically: start=jsb.first, sequence=this_seq (so the walker's
     * first-expected-seq matches the descriptor's seq). */
    rc = update_jsb(fs, this_first, this_seq);
    if (rc) { say(err, err_len, "jsb start/seq update failed"); return rc; }

    /* Step 6: populate the in-memory replay map directly — we know
     * exactly which fs_block went to which journal_blk, no need to
     * re-walk the log we just wrote. */
    fs->jbd.start         = this_first;
    fs->jbd.replay_count  = (uint32_t)trans->block_count;
    for (i = 0; i < trans->block_count; i++) {
        fs->jbd.replay[i].fs_block    = trans->fs_block[i];
        fs->jbd.replay[i].journal_blk = this_first + 1u + i;
        fs->jbd.replay[i].is_escape   = 0;
    }
    fs->jbd.replay_active = 1;

    /* Step 7: checkpoint — flushes data to fs_blocks, zeros jsb.start,
     * clears RECOVER. After this the FS is clean. */
    rc = ext4_journal_checkpoint(fs, err, err_len);
    if (rc) return rc;

    /* Step 8: bump in-memory sequence so future commits use seq+1. The
     * on-disk jsb.sequence stays at this_seq (from step 5) — that's
     * lwext4's "last_trans_id" convention. The next commit will overwrite
     * jsb.sequence to its own (higher) seq in step 5. */
    fs->jbd.sequence = this_seq + 1u;

    return 0;
}

int ext4_journal_checkpoint(struct ext4_fs *fs, char *err, uint32_t err_len) {
    uint32_t i;
    int      rc;

    if (err && err_len) err[0] = '\0';
    if (!fs->jbd.replay_active) return 0;
    if (!bdev_writable(fs->bd)) {
        say(err, err_len, "bdev read-only; staying in soft-replay");
        return -3; /* matches BDEV_ERR_RO */
    }

    /* 1. Flush each journaled block to its on-disk location. */
    for (i = 0; i < fs->jbd.replay_count; i++) {
        rc = checkpoint_one_block(fs,
                                  fs->jbd.replay[i].fs_block,
                                  fs->jbd.replay[i].journal_blk,
                                  fs->jbd.replay[i].is_escape);
        if (rc) {
            snprintf(err, err_len, "checkpoint flush failed at #%lu (rc=%d)",
                     (unsigned long)i, rc);
            return rc;
        }
    }

    /* 2. Clean the journal head. Leave jsb.sequence as-is — its value
     * only matters when start != 0. */
    rc = update_jsb(fs, 0u, fs->jbd.sequence);
    if (rc) { say(err, err_len, "jsb rewrite failed"); return rc; }

    /* 3. Clear RECOVER on the FS SB. */
    rc = update_fs_sb_recover(fs, 0);
    if (rc) { say(err, err_len, "fs sb rewrite failed"); return rc; }

    /* 4. Drop the in-memory replay state. Reads now go straight to disk. */
    fs->jbd.start            = 0;
    fs->jbd.replay_count     = 0;
    fs->jbd.revoke_count     = 0;
    fs->jbd.replay_active    = 0;
    fs->sb.feature_incompat &= ~(uint32_t)0x4u;
    return 0;
}

int ext4_journal_init(struct ext4_fs *fs, char *err, uint32_t err_len) {
    /* DGROUP static — see file header. */
    static struct ext4_inode jinode;

    uint32_t inum;
    uint16_t magic, entries, depth;
    int      rc;
    uint32_t i;
    uint64_t first_phys_block;
    uint32_t byte, sector, sectors;
    uint32_t blocktype;
    uint32_t unsupported;

    fs->jbd.present       = 0;
    fs->jbd.replay_active = 0;
    fs->jbd.extent_count  = 0;
    fs->jbd.replay_count  = 0;
    fs->jbd.revoke_count  = 0;

    inum = fs->sb.journal_inum;
    if (inum == 0) return 0;

    rc = ext4_inode_read(fs, inum, &jinode);
    if (rc) {
        snprintf(err, err_len, "journal inode read failed (rc=%d)", rc);
        return -1;
    }

    /* Only depth==0 extent trees handled. e2fsprogs lays out the journal
     * as a single extent at mkfs time, so this is the common case. */
    magic   = le16(jinode.i_block + 0);
    entries = le16(jinode.i_block + 2);
    depth   = le16(jinode.i_block + 6);

    if (magic != EXT4_EXT_MAGIC) {
        snprintf(err, err_len, "journal inode not extent-format (magic=0x%x)",
                 (unsigned)magic);
        return -2;
    }
    if (depth != 0) {
        snprintf(err, err_len, "journal extent tree depth %u unsupported",
                 (unsigned)depth);
        return -3;
    }
    if (entries == 0 || entries > EXT4_JBD_EXTENT_MAX) {
        snprintf(err, err_len, "journal extent count %u out of range",
                 (unsigned)entries);
        return -4;
    }

    fs->jbd.extent_count = entries;
    for (i = 0; i < entries; i++) {
        const uint8_t *ext = jinode.i_block + 12u + i * 12u;
        uint16_t real_len = (uint16_t)(le16(ext + 4) & 0x7FFFu);
        fs->jbd.extents[i].logical  = le32(ext + 0);
        fs->jbd.extents[i].length   = real_len;
        fs->jbd.extents[i].physical = ((uint64_t)le16(ext + 6) << 32)
                                    | (uint64_t)le32(ext + 8);
    }

    if (fs->jbd.extents[0].logical != 0) {
        say(err, err_len, "journal block 0 missing from extent[0]");
        return -5;
    }
    first_phys_block = fs->jbd.extents[0].physical;

    if (fs->sb.block_size > sizeof jsb_buf) {
        snprintf(err, err_len, "fs block_size %lu > jsb buf",
                 (unsigned long)fs->sb.block_size);
        return -6;
    }
    byte    = (uint32_t)(first_phys_block * (uint64_t)fs->sb.block_size);
    sector  = (uint32_t)(fs->partition_lba + byte / fs->bd->sector_size);
    sectors = fs->sb.block_size / fs->bd->sector_size;
    rc = bdev_read(fs->bd, sector, sectors, jsb_buf);
    if (rc) {
        snprintf(err, err_len, "journal sb read failed (rc=%d)", rc);
        return -7;
    }

    if (be32(jsb_buf + 0x00) != EXT4_JBD_MAGIC) {
        say(err, err_len, "journal superblock bad magic");
        return -8;
    }
    blocktype = be32(jsb_buf + 0x04);
    if (blocktype != EXT4_JBD_BT_SUPERBLOCK && blocktype != EXT4_JBD_BT_SUPER_V2) {
        snprintf(err, err_len, "journal sb blocktype %lu unexpected",
                 (unsigned long)blocktype);
        return -9;
    }

    fs->jbd.blocksize = be32(jsb_buf + 0x0C);
    fs->jbd.maxlen    = be32(jsb_buf + 0x10);
    fs->jbd.first     = be32(jsb_buf + 0x14);
    fs->jbd.sequence  = be32(jsb_buf + 0x18);
    fs->jbd.start     = be32(jsb_buf + 0x1C);

    if (blocktype == EXT4_JBD_BT_SUPER_V2) {
        fs->jbd.feature_incompat = be32(jsb_buf + 0x28);
        memcpy(fs->jbd.uuid, jsb_buf + 0x30, 16);
    } else {
        fs->jbd.feature_incompat = 0;
        memset(fs->jbd.uuid, 0, 16);
    }

    if (fs->jbd.blocksize != fs->sb.block_size) {
        snprintf(err, err_len, "journal blocksize %lu != fs %lu",
                 (unsigned long)fs->jbd.blocksize,
                 (unsigned long)fs->sb.block_size);
        return -10;
    }

    /* ASYNC_COMMIT changes commit-block placement in ways the walker
     * misinterprets. Refuse it. CSUM_V2/V3 we tolerate by ignoring (commit
     * block CRC verification is a later phase). REVOKE / 64BIT are normal. */
    unsupported = fs->jbd.feature_incompat & ~((uint32_t)(
        EXT4_JBD_INCOMPAT_REVOKE  |
        EXT4_JBD_INCOMPAT_64BIT   |
        EXT4_JBD_INCOMPAT_CSUM_V2 |
        EXT4_JBD_INCOMPAT_CSUM_V3));
    if (unsupported) {
        snprintf(err, err_len, "journal incompat 0x%lx unsupported",
                 (unsigned long)unsupported);
        return -11;
    }

    fs->jbd.csum_v2   = (uint8_t)((fs->jbd.feature_incompat & EXT4_JBD_INCOMPAT_CSUM_V2) ? 1 : 0);
    fs->jbd.csum_v3   = (uint8_t)((fs->jbd.feature_incompat & EXT4_JBD_INCOMPAT_CSUM_V3) ? 1 : 0);
    fs->jbd.has_64bit = (uint8_t)((fs->jbd.feature_incompat & EXT4_JBD_INCOMPAT_64BIT)   ? 1 : 0);
    fs->jbd.inum      = inum;
    fs->jbd.present   = 1;

    if (err && err_len) err[0] = '\0';
    return 0;
}
