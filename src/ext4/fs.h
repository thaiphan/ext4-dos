#ifndef EXT4_FS_H
#define EXT4_FS_H

#include <stdint.h>
#include "superblock.h"
#include "journal.h"

/* Static cap on the cached block-group descriptor table.
 * 4 KiB buffer holds 64 BGDs at 64 bytes each, or 128 BGDs at 32 bytes.
 * That's enough for several hundred MiB of filesystem at our v1 block sizes. */
#define EXT4_FS_BGD_BUF_SIZE 4096

struct blockdev;

struct ext4_fs {
    struct blockdev       *bd;
    uint64_t               partition_lba;        /* sectors */
    struct ext4_superblock sb;
    uint8_t                bgd_buf[EXT4_FS_BGD_BUF_SIZE];
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

#endif
