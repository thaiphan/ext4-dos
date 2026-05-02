#ifndef EXT4_EXTENT_H
#define EXT4_EXTENT_H

#include <stdint.h>

#define EXT4_EXT_MAGIC      0xF30Au
#define EXT4_EXT_NODE_BUF   4096   /* upper bound on FS block size we read */

struct ext4_fs;
struct ext4_inode;

/* Find the physical block backing a logical block in this inode's extent tree.
 * The initial node is the 60-byte i_block area of the inode (header + 4 entries).
 * Returns 0 on success and stores the physical block in *out_phys.
 * Returns negative on error (bad magic, hole, etc.). */
int ext4_extent_lookup(struct ext4_fs *fs, const uint8_t *initial_node,
                       uint32_t logical, uint64_t *out_phys);

/* Read one filesystem block of an inode's logical data. Buffer must be at
 * least sb.block_size bytes. */
int ext4_file_read_block(struct ext4_fs *fs, const struct ext4_inode *inode,
                         uint32_t logical_block, void *out_buf);

/* Read up to `length` bytes of the inode's data starting at offset 0.
 * Caps at the inode's size. Returns bytes read on success, negative on error.
 * Does not require the buffer to be a multiple of block_size. */
int ext4_file_read_head(struct ext4_fs *fs, const struct ext4_inode *inode,
                        uint32_t length, void *out_buf);

/* In-place file-block write through a journaled transaction. The
 * logical block must be backed by an existing extent — phase 1 doesn't
 * allocate. The supplied buffer must be exactly fs->sb.block_size bytes;
 * caller-supplied data is copied into a stable DGROUP buffer before the
 * commit, so the caller's buffer can be a transient stack/user-space
 * area. The inode's mtime is bumped to `now_unix` and persisted.
 *
 * Returns 0 on success. err is filled on failure with a short reason
 * (e.g. "block matches JBD_MAGIC", "logical block has no extent —
 * allocation is phase 2"). */
int ext4_file_write_block(struct ext4_fs *fs, struct ext4_inode *inode_in,
                          uint32_t inode_num, uint32_t logical_block,
                          const void *new_data, uint32_t now_unix,
                          char *err, uint32_t err_len);

/* Append exactly one new block to the end of the file, populating it
 * with `new_data` (block_size bytes). Phase 2 first cut: only the
 * "extend last leaf extent by 1" path is supported, and only when the
 * physically-next block (last_extent.physical + last_extent.length)
 * is free in group 0's bitmap. Builds a single 5-block transaction
 * (data + bitmap + BGD + fs sb + inode) and commits it.
 *
 * Refuses (with err) when:
 *   - file is empty (no extent to extend; use ext4_file_write_block
 *     after explicit allocate — not yet covered)
 *   - inode's extent tree has depth > 0 (deeper trees not handled)
 *   - inode has more than one leaf extent (would need a new leaf or
 *     to walk to last leaf — not yet handled)
 *   - the contiguous candidate block isn't free (would need to scan
 *     other groups — not yet handled)
 *   - the candidate block is in a group with BLOCK_UNINIT bitmap
 *
 * The inode's size is bumped by exactly one block; mtime is set to
 * `now_unix`; the in-memory inode_in is updated to reflect both. */
int ext4_file_extend_block(struct ext4_fs *fs, struct ext4_inode *inode_in,
                           uint32_t inode_num, const void *new_data,
                           uint32_t now_unix, char *err, uint32_t err_len);

#endif
