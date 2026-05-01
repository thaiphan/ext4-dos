#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "blockdev/blockdev.h"
#include "blockdev/file_bdev.h"
#include "ext4/superblock.h"
#include "ext4/features.h"
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
    uint64_t total_bytes = sb->blocks_count * (uint64_t)sb->block_size;
    printf("  volume name      : %s\n", sb->volume_name[0] ? sb->volume_name : "(unset)");
    printf("  last mounted     : %s\n", sb->last_mounted[0] ? sb->last_mounted : "(never)");
    printf("  uuid             : ");
    print_uuid(sb->uuid);
    printf("\n");
    printf("  block size       : %u\n", sb->block_size);
    printf("  blocks (total)   : %llu\n", (unsigned long long)sb->blocks_count);
    printf("  blocks (free)    : %llu\n", (unsigned long long)sb->free_blocks_count);
    printf("  inodes (total)   : %u\n", sb->inodes_count);
    printf("  inodes (free)    : %u\n", sb->free_inodes_count);
    printf("  inode size       : %u\n", sb->inode_size);
    printf("  size             : %llu MiB\n",
           (unsigned long long)(total_bytes / (1024u * 1024u)));
    printf("  state            : 0x%04x%s\n",
           sb->state, (sb->state & 1u) ? " (clean)" : "");
    printf("  feature_compat   : 0x%08x\n", sb->feature_compat);
    printf("  feature_incompat : 0x%08x\n", sb->feature_incompat);
    printf("  feature_ro_compat: 0x%08x\n", sb->feature_ro_compat);
}

static int inspect_at(struct blockdev *bd, uint64_t partition_lba) {
    /* Superblock lives at byte 1024 of the partition.
     * For 512-byte sectors that's sectors [partition_lba+2 .. partition_lba+3]. */
    uint8_t buf[1024];
    int rc = bdev_read(bd, partition_lba + 2, 2, buf);
    if (rc) {
        printf("  read superblock failed (%d)\n", rc);
        return -1;
    }
    struct ext4_superblock sb;
    if (ext4_superblock_parse(buf, &sb) != 0) {
        printf("  not ext4 (magic 0x%04x, expected 0x%04x)\n",
               sb.magic, (unsigned)EXT4_SUPERBLOCK_MAGIC);
        return -1;
    }
    printf("ext4 detected\n");
    print_summary(&sb);

    char err[160];
    if (ext4_features_check_supported(&sb, err, sizeof err) != 0) {
        printf("  v1 status        : REFUSED — %s\n", err);
        return 1;
    }
    printf("  v1 status        : supported\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: host_cli <ext4-image-or-disk>\n");
        return 1;
    }
    struct blockdev *bd = file_bdev_open(argv[1]);
    if (!bd) {
        fprintf(stderr, "open '%s' failed\n", argv[1]);
        return 1;
    }

    struct mbr_table mbr;
    int rc = mbr_read(bd, &mbr);
    if (rc == 0 && mbr.has_signature) {
        printf("MBR detected, %d partition(s):\n", mbr.count);
        for (int i = 0; i < mbr.count; i++) {
            const struct mbr_partition *p = &mbr.entries[i];
            printf("  [%d] type=0x%02x start=%u sectors=%u%s\n",
                   i, p->type, p->start_lba, p->sector_count,
                   p->bootable ? " bootable" : "");
        }
        for (int i = 0; i < mbr.count; i++) {
            const struct mbr_partition *p = &mbr.entries[i];
            if (p->type != MBR_TYPE_LINUX) continue;
            printf("\n=== Partition %d (Linux, LBA %u) ===\n", i, p->start_lba);
            inspect_at(bd, (uint64_t)p->start_lba);
        }
    } else {
        printf("(no MBR signature; treating as bare ext4 filesystem)\n\n");
        inspect_at(bd, 0);
    }

    file_bdev_close(bd);
    return 0;
}
