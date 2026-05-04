#ifndef EXT4_FS_H
#define EXT4_FS_H

#include <stdint.h>
#include "superblock.h"
#include "journal.h"

/* Single-FS-block BGD scratch. Big filesystems (>~64 groups at 4 KiB blocks
 * = >8 GiB) have BGD tables that span multiple FS blocks. ext4_fs_bgd_get
 * reads the right block on demand and tracks which one is currently
 * resident via bgd_cached_block (= 0xFFFFFFFFu when invalid). After any
 * write that updates a BGD's contents on disk, callers MUST call
 * ext4_fs_bgd_invalidate so the next read pulls fresh data. */
#define EXT4_FS_BGD_BUF_SIZE 4096
#define EXT4_FS_BGD_INVALID  0xFFFFFFFFu

struct blockdev;

struct ext4_fs {
    struct blockdev       *bd;
    uint64_t               partition_lba;        /* sectors */
    struct ext4_superblock sb;
    uint8_t                bgd_buf[EXT4_FS_BGD_BUF_SIZE];
    uint32_t               bgd_cached_block;     /* fs_block index in bgd_buf, or EXT4_FS_BGD_INVALID */
    uint32_t               bgd_count;
    uint16_t               bgd_size;             /* bytes per BGD entry: 32 or 64 */
    struct ext4_jbd        jbd;                  /* journal handle (present iff sb.journal_inum != 0) */
};

int  ext4_fs_open(struct ext4_fs *fs, struct blockdev *bd, uint64_t partition_lba);
void ext4_fs_close(struct ext4_fs *fs);

/* Read one full FS block (block_size bytes) into out_buf, redirecting
 * through the journal replay map if a newer committed copy exists.
 * Use this for any post-mount FS-block read (inode tables, extent
 * nodes, file/dir data). The pre-mount paths (sb, bgd) and the journal
 * itself stay on raw bdev_read. */
int ext4_fs_read_block(struct ext4_fs *fs, uint64_t fs_block, void *out_buf);

/* Return a pointer to the requested group's BGD entry, reading the
 * containing FS block into fs->bgd_buf if it isn't already cached.
 * Returns NULL on bdev failure or if group >= bgd_count. The returned
 * pointer is valid only until the next ext4_fs_bgd_get / _invalidate /
 * any other call that may indirectly trigger a re-read; callers that
 * need values past such a call must copy out fields locally. */
const uint8_t *ext4_fs_bgd_get(struct ext4_fs *fs, uint32_t group);

/* Drop the cached BGD block. Call after any commit that updates a BGD
 * on disk so the next read sees fresh data. Cheap (one field write). */
void ext4_fs_bgd_invalidate(struct ext4_fs *fs);

#endif
