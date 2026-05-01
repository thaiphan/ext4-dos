#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "blockdev/blockdev.h"
#include "blockdev/int13_bdev.h"
#include "ext4/superblock.h"
#include "ext4/features.h"
#include "partition/mbr.h"

static void print_uuid(const uint8_t *u) {
    int i;
    for (i = 0; i < 16; i++) {
        printf("%02x", u[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) printf("-");
    }
}

static void print_summary(const struct ext4_superblock *sb) {
    printf("  volume     : %s\n", sb->volume_name[0] ? sb->volume_name : "(unset)");
    printf("  uuid       : ");
    print_uuid(sb->uuid);
    printf("\n");
    printf("  block size : %lu\n", (unsigned long)sb->block_size);
    printf("  blocks     : %lu\n", (unsigned long)sb->blocks_count);
    printf("  inodes     : %lu\n", (unsigned long)sb->inodes_count);
    printf("  inode size : %u\n", sb->inode_size);
    printf("  state      : 0x%04x%s\n", sb->state, (sb->state & 1) ? " (clean)" : "");
    printf("  feat_incompat : 0x%08lx\n", (unsigned long)sb->feature_incompat);
}

static int inspect_at(struct blockdev *bd, uint32_t partition_lba) {
    static uint8_t buf[1024];
    struct ext4_superblock sb;
    char err[100];
    int rc;

    rc = bdev_read(bd, (uint64_t)partition_lba + 2, 2, buf);
    if (rc) {
        printf("  read superblock failed (%d)\n", rc);
        return -1;
    }
    if (ext4_superblock_parse(buf, &sb) != 0) {
        printf("  not ext4 (magic 0x%04x)\n", sb.magic);
        return -1;
    }
    printf("ext4 detected\n");
    print_summary(&sb);

    if (ext4_features_check_supported(&sb, err, sizeof err) != 0) {
        printf("  v1 status  : REFUSED -- %s\n", err);
        return 1;
    }
    printf("  v1 status  : supported\n");
    return 0;
}

int main(int argc, char **argv) {
    uint8_t drive = 0x80;
    struct blockdev *bd;
    struct mbr_table mbr;
    int rc;
    int i;

    if (argc >= 2) {
        drive = (uint8_t)strtoul(argv[1], NULL, 0);
    }

    printf("ext4-dos cli (DOS, drive 0x%02x)\n", drive);

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
            inspect_at(bd, p->start_lba);
        }
    } else {
        printf("(no MBR; treating drive 0x%02x as bare ext4)\n\n", drive);
        inspect_at(bd, 0);
    }

    int13_bdev_close(bd);
    return 0;
}
