#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#include "blockdev/blockdev.h"
#include "blockdev/int13_bdev.h"
#include "ext4/superblock.h"
#include "ext4/features.h"
#include "ext4/fs.h"
#include "ext4/inode.h"
#include "ext4/extent.h"
#include "ext4/dir.h"
#include "partition/mbr.h"

static void print_uuid(const uint8_t *u) {
    int i;
    for (i = 0; i < 16; i++) {
        printf("%02x", u[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) printf("-");
    }
}

static void print_hex_dump(const uint8_t *data, uint32_t len) {
    uint32_t i;
    uint32_t j;
    for (i = 0; i < len; i += 16) {
        printf("    %04lx: ", (unsigned long)i);
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

static int list_cb(const struct ext4_dir_entry *e, void *ud) {
    const char *kind = "?";
    (void)ud;
    switch (e->file_type) {
    case EXT4_FT_REGULAR: kind = "f"; break;
    case EXT4_FT_DIR:     kind = "d"; break;
    case EXT4_FT_SYMLINK: kind = "l"; break;
    }
    printf("    %s %6lu  %s\n", kind, (unsigned long)e->inode, e->name);
    return 0;
}

static int dump_inode(struct ext4_fs *fs, uint32_t ino) {
    static uint8_t blk[EXT4_EXT_NODE_BUF];
    struct ext4_inode inode;
    uint32_t bs;
    uint32_t dump_len;
    int rc;

    printf("\nInode %lu:\n", (unsigned long)ino);
    rc = ext4_inode_read(fs, ino, &inode);
    if (rc) {
        printf("  read failed (%d)\n", rc);
        return -1;
    }
    printf("  kind   : %s (mode 0%o)\n", mode_kind(inode.mode), inode.mode & 0xFFF);
    printf("  size   : %lu bytes\n", (unsigned long)inode.size);
    printf("  flags  : 0x%lx\n", (unsigned long)inode.flags);

    if (inode.size == 0) return 0;

    if ((inode.mode & EXT4_S_IFMT) == EXT4_S_IFDIR) {
        printf("  entries:\n");
        ext4_dir_iter(fs, &inode, list_cb, NULL);
        return 0;
    }

    bs = fs->sb.block_size;
    if (bs > sizeof blk) {
        printf("  (block size too large)\n");
        return 0;
    }
    rc = ext4_file_read_block(fs, &inode, 0, blk);
    if (rc) {
        printf("  read block 0 failed (%d)\n", rc);
        return -1;
    }
    dump_len = (uint32_t)((inode.size < (uint64_t)bs) ? inode.size : bs);
    if (dump_len > 256) dump_len = 256;
    printf("  first block (%lu bytes shown):\n", (unsigned long)dump_len);
    print_hex_dump(blk, dump_len);
    return 0;
}

/* File-scope static so we don't put a ~4 KB struct on the DOS stack
 * (default OpenWatcom small-model stack is ~4 KB; would overflow). */
static struct ext4_fs g_fs;

static void inspect_at(struct blockdev *bd, uint64_t partition_lba,
                       uint32_t inode_num, const char *path) {
    struct ext4_fs *fs = &g_fs;
    char err[100];
    int rc;

    rc = ext4_fs_open(fs, bd, partition_lba);
    if (rc) {
        printf("  fs_open failed (%d)\n", rc);
        return;
    }

    printf("ext4 detected\n");
    printf("  volume     : %s\n",
           fs->sb.volume_name[0] ? fs->sb.volume_name : "(unset)");
    printf("  uuid       : ");
    print_uuid(fs->sb.uuid);
    printf("\n");
    printf("  block size : %lu\n", (unsigned long)fs->sb.block_size);
    printf("  blocks     : %lu\n", (unsigned long)fs->sb.blocks_count);
    printf("  inodes     : %lu\n", (unsigned long)fs->sb.inodes_count);
    printf("  feat_inc   : 0x%08lx\n", (unsigned long)fs->sb.feature_incompat);

    if (ext4_features_check_supported(&fs->sb, err, sizeof err) != 0) {
        printf("  v1 status  : REFUSED -- %s\n", err);
        ext4_fs_close(fs);
        return;
    }
    printf("  v1 status  : supported\n");

    if (path) {
        inode_num = ext4_path_lookup(fs, path);
        if (inode_num == 0) {
            printf("\npath '%s' not found\n", path);
            ext4_fs_close(fs);
            return;
        }
        printf("\npath '%s' -> inode %lu\n", path, (unsigned long)inode_num);
    }

    if (inode_num != 0) {
        dump_inode(fs, inode_num);
    }

    ext4_fs_close(fs);
}

int main(int argc, char **argv) {
    uint8_t drive = 0x80;
    uint32_t inode_num = 0;
    const char *path = NULL;
    struct blockdev *bd;
    struct mbr_table mbr;
    int rc;
    int i;

    if (argc >= 2) drive = (uint8_t)strtoul(argv[1], NULL, 0);
    if (argc >= 3) {
        if (argv[2][0] == '/') {
            path = argv[2];
        } else {
            inode_num = (uint32_t)strtoul(argv[2], NULL, 0);
        }
    }

    printf("ext4-dos cli (DOS, drive 0x%02x)\n", drive);
    fflush(stdout);

    bd = int13_bdev_open(drive);
    if (!bd) {
        fprintf(stderr, "int13_bdev_open failed\n");
        return 1;
    }

    rc = mbr_read(bd, &mbr);
    if (rc == 0 && mbr.has_signature) {
        printf("MBR found, %d partition(s):\n", mbr.count);
        for (i = 0; i < mbr.count; i++) {
            const struct mbr_partition *p = &mbr.entries[i];
            printf("  [%d] type=0x%02x start=%lu sectors=%lu\n",
                   i, p->type, (unsigned long)p->start_lba,
                   (unsigned long)p->sector_count);
        }
        for (i = 0; i < mbr.count; i++) {
            const struct mbr_partition *p = &mbr.entries[i];
            if (p->type != MBR_TYPE_LINUX) continue;
            printf("\n=== Partition %d (Linux, LBA %lu) ===\n", i,
                   (unsigned long)p->start_lba);
            inspect_at(bd, (uint64_t)p->start_lba, inode_num, path);
        }
    } else {
        printf("(no MBR; treating drive 0x%02x as bare ext4)\n\n", drive);
        inspect_at(bd, 0, inode_num, path);
    }

    int13_bdev_close(bd);
    return 0;
}
