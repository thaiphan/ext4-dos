#ifndef EXT4_FEATURES_H
#define EXT4_FEATURES_H

#include <stdint.h>
#include <stddef.h>

/* feature_ro_compat bits — RO mount is only safe if we can read correctly
 * even when the writer assumed the feature. Safe-for-RO unless the feature
 * changes the on-disk layout we walk (block/extent/inode table addressing). */
#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER  0x0001u  /* sparse superblocks — RO ok */
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE    0x0002u  /* size > 2GB inodes — RO ok */
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR     0x0004u  /* unused */
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE     0x0008u  /* >2TiB files — RO ok (we read low 32 of size, fine for files <4GB) */
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM      0x0010u  /* GDT checksum — RO ok (we don't verify) */
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK     0x0020u  /* no 32K subdir limit — RO ok */
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE   0x0040u  /* big inodes — RO ok */
#define EXT4_FEATURE_RO_COMPAT_QUOTA         0x0100u  /* quota in special inodes — RO ok (we don't read them) */
#define EXT4_FEATURE_RO_COMPAT_BIGALLOC      0x0200u  /* multi-block clusters — UNSUPPORTED, breaks extent math */
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400u  /* metadata checksums — RO ok (we don't verify, and the data is still valid) */
#define EXT4_FEATURE_RO_COMPAT_REPLICA       0x0800u
#define EXT4_FEATURE_RO_COMPAT_READONLY      0x1000u
#define EXT4_FEATURE_RO_COMPAT_PROJECT       0x2000u  /* project quotas — RO ok */
#define EXT4_FEATURE_RO_COMPAT_VERITY        0x8000u  /* fs-verity — RO ok (we don't verify) */
#define EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT 0x20000u

#define EXT4_FEATURE_INCOMPAT_COMPRESSION 0x00000001u
#define EXT4_FEATURE_INCOMPAT_FILETYPE    0x00000002u
#define EXT4_FEATURE_INCOMPAT_RECOVER     0x00000004u
#define EXT4_FEATURE_INCOMPAT_JOURNAL_DEV 0x00000008u
#define EXT4_FEATURE_INCOMPAT_META_BG     0x00000010u
#define EXT4_FEATURE_INCOMPAT_EXTENTS     0x00000040u
/* INCOMPAT_64BIT is in superblock.h since the parser needs it */
#define EXT4_FEATURE_INCOMPAT_MMP         0x00000100u
#define EXT4_FEATURE_INCOMPAT_FLEX_BG     0x00000200u
#define EXT4_FEATURE_INCOMPAT_EA_INODE    0x00000400u
#define EXT4_FEATURE_INCOMPAT_DIRDATA     0x00001000u
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED   0x00002000u
#define EXT4_FEATURE_INCOMPAT_LARGEDIR    0x00004000u
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x00008000u
#define EXT4_FEATURE_INCOMPAT_ENCRYPT     0x00010000u
#define EXT4_FEATURE_INCOMPAT_CASEFOLD    0x00020000u

/* What v1 (read-only) supports.
 *
 * RECOVER (journal-needs-recovery) is intentionally allowed without
 * actually replaying the journal. Same approach GRUB's ext2 driver takes
 * for the same reason: a read-only mount that ignores the journal sees the
 * ON-DISK state, which is the state as of the last sync. The transactions
 * sitting in the journal are LOST writes — for files actively being
 * written when the host crashed, the user may see slightly stale content.
 * For typical use (reading existing data on a disk that's been idle),
 * this is fine. If we ever need post-replay correctness, implement real
 * journal replay (parse jbd2 transactions, build a {disk_blk -> jrnl_blk}
 * map, override block reads to consult it) — see references/lwext4 and
 * grub's ext2.c "needs_recovery" comment. INLINE_DATA / LARGEDIR /
 * ENCRYPT / CASEFOLD / EA_INODE / DIRDATA / MMP all deferred. */
#define EXT4_V1_INCOMPAT_SUPPORTED ( \
    EXT4_FEATURE_INCOMPAT_FILETYPE  | \
    EXT4_FEATURE_INCOMPAT_RECOVER   | \
    EXT4_FEATURE_INCOMPAT_META_BG   | \
    EXT4_FEATURE_INCOMPAT_EXTENTS   | \
    0x00000080u                     | /* 64BIT */ \
    EXT4_FEATURE_INCOMPAT_FLEX_BG   | \
    EXT4_FEATURE_INCOMPAT_CSUM_SEED )

/* Read-only mount ignores most ro_compat features (they're "RO" — safe to
 * READ around them, just unsafe to WRITE). BIGALLOC is the exception: it
 * changes block↔cluster sizing in a way that breaks our extent math, so
 * we refuse cleanly. */
#define EXT4_V1_RO_COMPAT_SUPPORTED ( \
    EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER  | \
    EXT4_FEATURE_RO_COMPAT_LARGE_FILE    | \
    EXT4_FEATURE_RO_COMPAT_BTREE_DIR     | \
    EXT4_FEATURE_RO_COMPAT_HUGE_FILE     | \
    EXT4_FEATURE_RO_COMPAT_GDT_CSUM      | \
    EXT4_FEATURE_RO_COMPAT_DIR_NLINK     | \
    EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE   | \
    EXT4_FEATURE_RO_COMPAT_QUOTA         | \
    EXT4_FEATURE_RO_COMPAT_METADATA_CSUM | \
    EXT4_FEATURE_RO_COMPAT_PROJECT       | \
    EXT4_FEATURE_RO_COMPAT_VERITY        | \
    EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT )

struct ext4_superblock;

int ext4_features_check_supported(const struct ext4_superblock *sb,
                                  char *err, size_t err_len);

#endif
