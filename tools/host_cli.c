#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#include "blockdev/blockdev.h"
#include "blockdev/file_bdev.h"
#include "ext4/superblock.h"
#include "ext4/features.h"
#include "ext4/fs.h"
#include "ext4/inode.h"
#include "ext4/extent.h"
#include "partition/mbr.h"

static void print_uuid(const uint8_t *u) {
    printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           u[0], u[1], u[2], u[3],
           u[4], u[5],
           u[6], u[7],
           u[8], u[9],
           u[10], u[11], u[12], u[13], u[14], u[15]);
}

static void print_summary(const struct ext4_superblock *sb) {
    printf("  volume name      : %s\n", sb->volume_name[0] ? sb->volume_name : "(unset)");
    printf("  uuid             : ");
    print_uuid(sb->uuid);
    printf("\n");
    printf("  block size       : %u\n", sb->block_size);
    printf("  blocks (total)   : %llu\n", (unsigned long long)sb->blocks_count);
    printf("  inodes (total)   : %u\n", sb->inodes_count);
    printf("  inode size       : %u\n", sb->inode_size);
    printf("  state            : 0x%04x%s\n",
           sb->state, (sb->state & 1u) ? " (clean)" : "");
    printf("  feature_incompat : 0x%08x\n", sb->feature_incompat);
}

static void print_hex_dump(const uint8_t *data, uint32_t len) {
    uint32_t i, j;
    for (i = 0; i < len; i += 16) {
        printf("    %04x: ", i);
        for (j = 0; j < 16 && i + j < len; j++) {
            printf("%02x ", data[i + j]);
        }
        for (; j < 16; j++) printf("   ");
        printf(" |");
        for (j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = data[i + j];
            putchar(isprint(c) ? c : '.');
        }
        printf("|\n");
    }
}

static const char *mode_kind(uint16_t mode) {
    switch (mode & EXT4_S_IFMT) {
    case EXT4_S_IFREG: return "regular";
    case EXT4_S_IFDIR: return "directory";
    case EXT4_S_IFLNK: return "symlink";
    default:           return "other";
    }
}

static int dump_inode(struct ext4_fs *fs, uint32_t ino) {
    struct ext4_inode inode;
    static uint8_t blk[EXT4_EXT_NODE_BUF];
    uint32_t bs;
    uint32_t dump_len;
    int rc;

    printf("\nInode %u:\n", ino);
    rc = ext4_inode_read(fs, ino, &inode);
    if (rc) {
        printf("  read failed (%d)\n", rc);
        return -1;
    }
    printf("  kind   : %s (mode 0%o)\n", mode_kind(inode.mode), inode.mode & 0xFFFu);
    printf("  size   : %llu bytes\n", (unsigned long long)inode.size);
    printf("  flags  : 0x%x%s\n", inode.flags,
           (inode.flags & EXT4_INODE_FLAG_EXTENTS) ? " (extents)" : "");

    if (inode.size == 0) return 0;

    bs = fs->sb.block_size;
    if (bs > sizeof blk) {
        printf("  (block size %u exceeds dump buffer)\n", bs);
        return 0;
    }
    rc = ext4_file_read_block(fs, &inode, 0, blk);
    if (rc) {
        printf("  read block 0 failed (%d)\n", rc);
        return -1;
    }
    dump_len = (uint32_t)((inode.size < (uint64_t)bs) ? inode.size : bs);
    if (dump_len > 256) dump_len = 256;
    printf("  first block (%u bytes shown):\n", dump_len);
    print_hex_dump(blk, dump_len);
    return 0;
}

static int inspect_at(struct blockdev *bd, uint64_t partition_lba, uint32_t inode_num) {
    struct ext4_fs fs;
    char err[160];
    int rc;

    rc = ext4_fs_open(&fs, bd, partition_lba);
    if (rc) {
        printf("  fs_open failed (%d)\n", rc);
        return -1;
    }

    printf("ext4 detected\n");
    print_summary(&fs.sb);

    if (ext4_features_check_supported(&fs.sb, err, sizeof err) != 0) {
        printf("  v1 status        : REFUSED -- %s\n", err);
        ext4_fs_close(&fs);
        return 1;
    }
    printf("  v1 status        : supported\n");

    if (inode_num != 0) {
        dump_inode(&fs, inode_num);
    }

    ext4_fs_close(&fs);
    return 0;
}

int main(int argc, char **argv) {
    struct blockdev *bd;
    struct mbr_table mbr;
    uint32_t inode_num = 0;
    int rc;
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: host_cli <ext4-image-or-disk> [inode-number]\n");
        return 1;
    }
    if (argc >= 3) {
        inode_num = (uint32_t)strtoul(argv[2], NULL, 0);
    }

    bd = file_bdev_open(argv[1]);
    if (!bd) {
        fprintf(stderr, "open '%s' failed\n", argv[1]);
        return 1;
    }

    rc = mbr_read(bd, &mbr);
    if (rc == 0 && mbr.has_signature) {
        printf("MBR detected, %d partition(s):\n", mbr.count);
        for (i = 0; i < mbr.count; i++) {
            const struct mbr_partition *p = &mbr.entries[i];
            printf("  [%d] type=0x%02x start=%u sectors=%u%s\n",
                   i, p->type, p->start_lba, p->sector_count,
                   p->bootable ? " bootable" : "");
        }
        for (i = 0; i < mbr.count; i++) {
            const struct mbr_partition *p = &mbr.entries[i];
            if (p->type != MBR_TYPE_LINUX) continue;
            printf("\n=== Partition %d (Linux, LBA %u) ===\n", i, p->start_lba);
            inspect_at(bd, (uint64_t)p->start_lba, inode_num);
        }
    } else {
        printf("(no MBR signature; treating as bare ext4 filesystem)\n\n");
        inspect_at(bd, 0, inode_num);
    }

    file_bdev_close(bd);
    return 0;
}
