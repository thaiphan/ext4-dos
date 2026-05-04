#ifndef EXT4_JOURNAL_H
#define EXT4_JOURNAL_H

#include <stdint.h>

/* jbd2 / journal-block-device-2 — the log format ext4 uses for crash
 * consistency. Spec: linux/Documentation/filesystems/ext4/journal.rst.
 * All multi-byte fields on disk are big-endian (the log was inherited
 * from JBD on PowerPC). Use util/endian.h's be16/be32/be64.
 *
 * Read-side soft replay: we parse the on-disk journal, build a
 * map {fs_block -> latest committed copy in the journal}, and have the
 * fs read path consult the map. We do not write anything. Compared to
 * GRUB's ext2 driver (which ignores the journal entirely) this gives
 * post-replay correctness without committing to a write path. */

#define EXT4_JBD_MAGIC        0xC03B3998u  /* JFS_MAGIC_NUMBER */

/* jbd_bhdr.blocktype values */
#define EXT4_JBD_BT_DESCRIPTOR 1u
#define EXT4_JBD_BT_COMMIT     2u
#define EXT4_JBD_BT_SUPERBLOCK 3u  /* v1 — pre-feature-flags */
#define EXT4_JBD_BT_SUPER_V2   4u  /* v2 — has feature flags + uuid */
#define EXT4_JBD_BT_REVOKE     5u

/* feature_incompat — features that, if unknown, mean we cannot replay safely */
#define EXT4_JBD_INCOMPAT_REVOKE      0x00000001u
#define EXT4_JBD_INCOMPAT_64BIT       0x00000002u  /* block tags carry blocknr_high */
#define EXT4_JBD_INCOMPAT_ASYNC_COMMIT 0x00000004u
#define EXT4_JBD_INCOMPAT_CSUM_V2     0x00000008u  /* per-tag crc16, descriptor-tail crc32c */
#define EXT4_JBD_INCOMPAT_CSUM_V3     0x00000010u  /* tag3 with full crc32c */

/* descriptor-block-tag flags */
#define EXT4_JBD_TAG_FLAG_ESCAPE     0x01u  /* block's first u32 was JBD_MAGIC; restore on replay */
#define EXT4_JBD_TAG_FLAG_SAME_UUID  0x02u  /* tag omits uuid (use previous) */
#define EXT4_JBD_TAG_FLAG_DELETED    0x04u
#define EXT4_JBD_TAG_FLAG_LAST_TAG   0x08u

/* Capacity caps — these gate how much memory the journal handle holds in
 * DGROUP. A clean (non-dirty) journal needs almost nothing; only a
 * RECOVER-flagged mount actually populates the maps. Going past these
 * caps aborts replay (we fall back to on-disk state with a clear error,
 * not silent corruption).
 *
 * Sized down from earlier (256/64/16) once the write path added 5 more
 * fs-block scratch buffers — DGROUP is finite in DOS small-model. A
 * journal flush of a few hundred journaled blocks is rare; if it
 * happens the walker bails and we serve on-disk state. */
#define EXT4_JBD_REPLAY_MAP_MAX   64u  /* unique fs_blocks redirected */
#define EXT4_JBD_REVOKE_MAP_MAX   32u  /* unique fs_blocks revoked */
#define EXT4_JBD_EXTENT_MAX        8u  /* extents in journal-inode i_block walk */

/* One entry in the replay map: an fs_block whose newest committed copy
 * lives at journal block journal_blk. Sorted by fs_block for binary search. */
struct ext4_jbd_replay_entry {
    uint64_t fs_block;
    uint32_t journal_blk;   /* index into the journal log, 0..maxlen-1 */
    uint8_t  is_escape;     /* on read, restore first 4 bytes to JBD magic */
    uint8_t  pad[3];
};

struct ext4_jbd_revoke_entry {
    uint64_t fs_block;
    uint32_t revoke_seq;    /* transactions older-or-equal to this are skipped */
};

/* Snapshot of the journal inode's extent layout. Lets us translate a
 * journal-block index (0..maxlen-1) to an absolute fs_block without
 * re-walking the extent tree on every read. */
struct ext4_jbd_extent {
    uint32_t logical;       /* journal-block index this extent starts at */
    uint32_t length;        /* in journal blocks */
    uint64_t physical;      /* absolute fs_block where this extent's data lives */
};

struct ext4_jbd {
    uint8_t  present;       /* 0 = no journal handle (no s_journal_inum) */
    uint8_t  replay_active; /* 0 = clean mount, no redirect needed */
    uint8_t  pad[2];

    uint32_t inum;          /* journal inode number */
    uint32_t blocksize;     /* journal block size; matches fs block_size */
    uint32_t maxlen;        /* total blocks in journal */
    uint32_t first;         /* first log block (after the journal superblock) */
    uint32_t sequence;      /* first-expected commit id */
    uint32_t start;         /* log start block; 0 means clean — no replay needed */

    uint32_t feature_incompat;
    uint8_t  uuid[16];      /* journal-internal uuid; CRC32C seed */
    uint8_t  csum_v2;       /* derived: incompat has CSUM_V2 */
    uint8_t  csum_v3;       /* derived: incompat has CSUM_V3 */
    uint8_t  has_64bit;     /* derived: tag carries blocknr_high */
    uint8_t  pad2;

    uint32_t extent_count;
    struct ext4_jbd_extent extents[EXT4_JBD_EXTENT_MAX];

    /* Populated by replay walk; consulted by reads. Both are kept
     * sorted by fs_block. Counts are 0 on clean mount. */
    uint32_t replay_count;
    struct ext4_jbd_replay_entry replay[EXT4_JBD_REPLAY_MAP_MAX];

    uint32_t revoke_count;
    struct ext4_jbd_revoke_entry revoke[EXT4_JBD_REVOKE_MAP_MAX];
};

struct ext4_fs;

/* Read inode #s_journal_inum, parse the journal superblock, populate
 * jbd->extents/maxlen/start/sequence. Does NOT walk transactions
 * (that's ext4_journal_replay). Returns 0 on success, negative on
 * error. If sb has no journal_inum, returns 0 with jbd->present == 0.
 * If parsing fails, returns negative AND fills err with a short reason. */
int ext4_journal_init(struct ext4_fs *fs, char *err, uint32_t err_len);

/* Walk the on-disk log and apply replay. Behavior depends on bdev:
 *
 *  - Writable bdev: streaming flush. Each non-revoked tag's data block
 *    is written straight to its fs_block during the walk; cleanup
 *    (jsb.start=0, clear RECOVER) is delegated to the post-replay
 *    ext4_journal_checkpoint call. No in-memory replay map is built,
 *    so EXT4_JBD_REPLAY_MAP_MAX does NOT cap the journal size.
 *
 *  - Read-only bdev: soft replay. Builds {fs_block -> journal_blk}
 *    map (and revoke map); the read hook redirects reads of journaled
 *    blocks to their newest committed copy in the log. The map is
 *    capped at EXT4_JBD_REPLAY_MAP_MAX entries; overflow returns
 *    negative — ext4_fs_open propagates the error and refuses the
 *    mount rather than serving pre-replay data.
 *
 * No-op if jbd->present == 0 or jbd->start == 0 (clean log). Returns
 * 0 on success. On streaming-flush write failure, returns negative
 * with RECOVER still set on disk — next mount retries idempotently. */
int ext4_journal_replay(struct ext4_fs *fs, char *err, uint32_t err_len);

/* "Hard replay": apply the replay map to disk so the on-disk state
 * matches the journaled state, then mark the journal clean (s_start=0,
 * recompute jsb checksum) and clear EXT4_FEATURE_INCOMPAT_RECOVER on
 * the FS superblock (recompute fs sb checksum if metadata_csum is on).
 * After this the FS is e2fsck-clean and a future mount sees no journal
 * to replay.
 *
 * No-op if replay_active == 0. Returns BDEV_ERR_RO if bdev is
 * read-only — caller stays in soft-replay mode. */
int ext4_journal_checkpoint(struct ext4_fs *fs, char *err, uint32_t err_len);

/* Look up fs_block in the replay map. Returns 1 with *out_jblk and
 * *out_is_escape filled in if a journaled copy exists; 0 otherwise. */
int ext4_journal_lookup(const struct ext4_fs *fs, uint64_t fs_block,
                        uint32_t *out_jblk, uint8_t *out_is_escape);

/* A pending transaction. Caller fills block_count/fs_block/buf and
 * passes to ext4_journal_commit. Buffers must remain live until commit
 * returns. Cap is small — the in-place file write call site uses 2
 * (data + inode metadata). */
#define EXT4_JBD_TRANS_MAX_BLOCKS 8
struct ext4_jbd_trans {
    uint32_t block_count;
    uint64_t fs_block[EXT4_JBD_TRANS_MAX_BLOCKS];
    void    *buf[EXT4_JBD_TRANS_MAX_BLOCKS];
};

/* Emit a jbd2 transaction for the listed blocks: descriptor + data
 * blocks + commit, all with valid CSUM_V2/V3 checksums if the journal
 * uses them, then immediately checkpoint (flush data to fs_blocks,
 * clean jsb, clear RECOVER).
 *
 * Requires:
 *   - jsb.start must be 0 on entry (clean journal — caller should mount
 *     writable so any pending replay is checkpointed first).
 *   - Callers are responsible for recomputing inode/block-group
 *     checksums on metadata blocks before passing them here.
 *
 * Returns 0 on success. err is filled with a short reason on failure. */
int ext4_journal_commit(struct ext4_fs *fs, struct ext4_jbd_trans *trans,
                        char *err, uint32_t err_len);

/* Read journal block jblk (a logical index into the log) into out_buf.
 * Translates via the journal inode's extent table. out_buf must be at
 * least block_size bytes. */
int ext4_journal_read_log_block(struct ext4_fs *fs, uint32_t jblk, void *out_buf);

#endif
