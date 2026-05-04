#ifndef EXT4_HTREE_H
#define EXT4_HTREE_H

#include <stdint.h>
#include "fs.h"
#include "inode.h"

/* htree (dir_index) write-side helpers. Read-side support is incidental:
 * ext4_dir_iter walks dir blocks linearly, and the htree root block's
 * fake `..` rec_len skips over the dx_root index, so a linear walker
 * naturally sees just `.`/`..` in the root and then iterates the leaf
 * blocks. The tricky part is INSERTING — Linux readers consult the
 * dx_root index by hash to find the right leaf, so we must insert into
 * that specific leaf or our entry won't be found.
 *
 * Hash version supported: HALF_MD4 (default for modern ext4 mkfs). TEA
 * and legacy hash versions are detected and refused — caller should
 * fall back to refusing the operation cleanly. */

#define EXT4_HTREE_HASH_LEGACY        0
#define EXT4_HTREE_HASH_HALF_MD4      1
#define EXT4_HTREE_HASH_TEA           2
#define EXT4_HTREE_HASH_LEGACY_UNSIGNED  3
#define EXT4_HTREE_HASH_HALF_MD4_UNSIGNED 4
#define EXT4_HTREE_HASH_TEA_UNSIGNED      5

/* Compute the htree "major" hash for a directory entry name.
 * Returns 0 on success, -1 on unsupported hash_version (caller should
 * refuse the operation). The "minor" hash is unused at level-0 lookup. */
int ext4_htree_hash_name(uint8_t hash_version, const uint32_t seed[4],
                         const char *name, uint8_t name_len,
                         uint32_t *out_hash_major);

/* Walk the root block's index to find the leaf logical-block index for
 * a given name. parent must be an htree dir (caller checks
 * EXT4_INODE_FLAG_INDEX).
 *
 * scratch must be a caller-owned buffer of at least fs->sb.block_size
 * bytes; ext4_htree_find_leaf reuses it to read both the FS superblock
 * (for s_hash_seed) and the dir's root block. On return, scratch's
 * contents are undefined — the caller can reuse it freely (e.g., to
 * read the leaf). Avoiding file-static buffers here keeps htree.o's
 * DGROUP footprint to zero, which matters: shifting DGROUP layout has
 * historically broken unrelated MS-DOS 4 paths (see
 * project_msdos4_size_sensitivity memory).
 *
 * Returns:
 *    0 on success — *out_leaf_logical is the leaf's logical block #
 *   -1 on read failure
 *   -2 on unsupported hash version (caller should refuse the op)
 *   -3 on multi-level htree (indirect_levels > 0; refused for v1)
 *   -4 on malformed root (bad limit/count, etc.) */
int ext4_htree_find_leaf(struct ext4_fs *fs,
                         const struct ext4_inode *parent,
                         const char *name, uint8_t name_len,
                         uint8_t *scratch,
                         uint32_t *out_leaf_logical);

#endif
