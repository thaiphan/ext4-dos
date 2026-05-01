#ifndef EXT4_FEATURES_H
#define EXT4_FEATURES_H

#include <stdint.h>
#include <stddef.h>

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
 * RECOVER deliberately excluded — needs journal replay (later milestone).
 * MMP excluded — means another system has it mounted; we should not race.
 * INLINE_DATA / LARGEDIR / ENCRYPT / CASEFOLD / EA_INODE / DIRDATA all
 * deferred to later milestones. */
#define EXT4_V1_INCOMPAT_SUPPORTED ( \
    EXT4_FEATURE_INCOMPAT_FILETYPE  | \
    EXT4_FEATURE_INCOMPAT_META_BG   | \
    EXT4_FEATURE_INCOMPAT_EXTENTS   | \
    0x00000080u                     | /* 64BIT */ \
    EXT4_FEATURE_INCOMPAT_FLEX_BG   | \
    EXT4_FEATURE_INCOMPAT_CSUM_SEED )

struct ext4_superblock;

int ext4_features_check_supported(const struct ext4_superblock *sb,
                                  char *err, size_t err_len);

#endif
