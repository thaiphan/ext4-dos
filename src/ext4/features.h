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
 * RECOVER (journal-needs-recovery) is allowed because src/ext4/journal.c
 * does soft replay at mount: it parses the on-disk jbd2 log, builds a
 * {fs_block -> journal_blk} map, and ext4_fs_read_block redirects reads
 * of journaled blocks to their newest committed copy. On-disk state is
 * never modified. If the walker hits its caps or sees a feature it
 * doesn't understand, replay aborts cleanly and reads fall back to the
 * pre-replay world (the same posture GRUB's ext2 driver takes — slightly
 * stale data for files written just before the crash, but otherwise
 * fine). INLINE_DATA / LARGEDIR / ENCRYPT / CASEFOLD / EA_INODE /
 * DIRDATA / MMP all deferred. */
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
