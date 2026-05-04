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
 * logical block must be backed by an existing extent — caller must
 * pre-allocate. The supplied buffer must be exactly fs->sb.block_size
 * bytes; caller-supplied data is copied into a stable DGROUP buffer
 * before the commit, so the caller's buffer can be a transient
 * stack/user-space area. The inode's mtime is bumped to `now_unix`
 * and persisted.
 *
 * Returns 0 on success. err is filled on failure with a short reason
 * (e.g. "block matches JBD_MAGIC", "logical block has no extent"). */
int ext4_file_write_block(struct ext4_fs *fs, struct ext4_inode *inode_in,
                          uint32_t inode_num, uint32_t logical_block,
                          const void *new_data, uint32_t now_unix,
                          char *err, uint32_t err_len);

/* Create a new regular file in the directory at parent_ino.
 * name must be null-terminated; name_len is strlen(name).
 * mode sets the file type bits (e.g. EXT4_S_IFREG | 0644).
 * Finds a free inode, initialises it, and inserts a new linear dir entry
 * into parent_ino's first data block that has room. Refuses if the
 * parent directory uses htree indexing (only linear dirs supported).
 * Returns the new inode number on success, 0 on failure with err set. */
uint32_t ext4_file_create(struct ext4_fs *fs, uint32_t parent_ino,
                          const char *name, uint8_t name_len,
                          uint16_t mode, uint32_t now_unix,
                          char *err, uint32_t err_len);

/* Append exactly one new block to the end of the file, populating it
 * with `new_data` (block_size bytes). Scans all block groups for a free
 * block, preferring the physically-contiguous candidate for locality.
 * Builds a 5-block transaction (data + bitmap + BGD + fs sb + inode)
 * and commits it.
 *
 * Refuses (with err) when:
 *   - inode's extent tree has depth > 0 (deeper trees not handled)
 *   - inode has more than 4 leaf extents AND the contiguous block is
 *     taken (no room for a new leaf entry)
 *   - no free block found in any group
 *
 * The inode's size is bumped by exactly one block; mtime is set to
 * `now_unix`; the in-memory inode_in is updated to reflect both. */
/* Create a new directory under parent_ino.  Allocates a fresh inode
 * (mode S_IFDIR|0755, links=2), writes a data block containing the
 * dot and dotdot entries, inserts a linear dir entry in parent_ino,
 * and bumps parent's link count.  Uses two journal transactions (one
 * for inode alloc, one for block alloc + wiring) so that
 * find_free_inode and find_free_block don't share scratch buffers.
 * Refuses htree parent directories (only linear dirs supported).
 * Returns new inode number on success, 0 on failure with err set. */
uint32_t ext4_dir_create(struct ext4_fs *fs, uint32_t parent_ino,
                         const char *name, uint8_t name_len,
                         uint32_t now_unix,
                         char *err, uint32_t err_len);

/* Rename a file or directory in-place within parent_ino.  Updates the
 * directory entry name to new_name (must fit within the entry's current
 * rec_len).  Same-parent only — cross-directory rename is not supported.
 * Returns 0 on success, -1 on failure. */
int ext4_rename(struct ext4_fs *fs, uint32_t parent_ino, uint32_t file_ino,
                const char *new_name, uint8_t new_name_len,
                char *err, uint32_t err_len);

/* Remove a regular file from parent_ino.  Frees each data block (one
 * 3-block journal transaction per block), then frees the inode + removes
 * the parent dir entry in a final 6-block transaction.  Refuses
 * directories (use ext4_dir_remove), depth>0 extent trees, and inode 0/2.
 * Returns 0 on success, -1 on failure. */
int ext4_file_remove(struct ext4_fs *fs, uint32_t parent_ino, uint32_t file_ino,
                     char *err, uint32_t err_len);

/* Truncate a regular file to new_size (must be ≤ current size).  Frees
 * data blocks past the new boundary one at a time (3-block tx each),
 * then commits a final 1-block tx writing the inode with the truncated
 * extent tree, updated size + blocks_count, bumped m/ctime, and a
 * recomputed i_checksum.  Refuses directories and depth>0 trees.
 * Returns 0 on success, -1 on failure. */
int ext4_file_truncate(struct ext4_fs *fs, uint32_t inode_num,
                       uint64_t new_size, uint32_t now_unix,
                       char *err, uint32_t err_len);

/* Remove an empty directory from parent_ino.  Verifies dir_ino is a
 * directory containing only dot/dotdot, frees the data block (tx1) and
 * inode (tx2), removes the entry from parent_ino, and decrements
 * parent's link count.  Returns 0 on success, -1 on failure. */
int ext4_dir_remove(struct ext4_fs *fs, uint32_t parent_ino, uint32_t dir_ino,
                    char *err, uint32_t err_len);

/* append_bytes: how many valid bytes are in new_data (≤ block_size).
 * Zeros fill the rest of the block on disk; inode.size advances by
 * append_bytes, not block_size. Pass block_size for a full-block extend. */
int ext4_file_extend_block(struct ext4_fs *fs, struct ext4_inode *inode_in,
                           uint32_t inode_num, const void *new_data,
                           uint32_t append_bytes,
                           uint32_t now_unix, char *err, uint32_t err_len);

#endif
