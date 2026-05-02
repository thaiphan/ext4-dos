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
 * (e.g. "metadata_csum write support is phase 1c", "block matches
 * JBD_MAGIC", "logical block has no extent — allocation is phase 2"). */
int ext4_file_write_block(struct ext4_fs *fs, struct ext4_inode *inode_in,
                          uint32_t inode_num, uint32_t logical_block,
                          const void *new_data, uint32_t now_unix,
                          char *err, uint32_t err_len);

#endif
