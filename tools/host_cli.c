#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "ext4/superblock.h"

static void print_uuid(const uint8_t *u) {
    printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           u[0], u[1], u[2], u[3],
           u[4], u[5],
           u[6], u[7],
           u[8], u[9],
           u[10], u[11], u[12], u[13], u[14], u[15]);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: host_cli <ext4-image>\n");
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "open '%s' failed\n", argv[1]);
        return 1;
    }
    if (fseek(f, EXT4_SUPERBLOCK_OFFSET, SEEK_SET) != 0) {
        fprintf(stderr, "seek to superblock failed\n");
        fclose(f);
        return 1;
    }
    uint8_t buf[EXT4_SUPERBLOCK_SIZE];
    size_t got = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (got != sizeof buf) {
        fprintf(stderr, "short read on superblock (%zu of %zu)\n", got, sizeof buf);
        return 1;
    }

    struct ext4_superblock sb;
    if (ext4_superblock_parse(buf, &sb) != 0) {
        fprintf(stderr, "not an ext4 filesystem (bad magic 0x%04x)\n", sb.magic);
        return 2;
    }

    uint64_t total_bytes = sb.blocks_count * (uint64_t)sb.block_size;

    printf("ext4 detected\n");
    printf("  volume name      : %s\n", sb.volume_name[0] ? sb.volume_name : "(unset)");
    printf("  last mounted     : %s\n", sb.last_mounted[0] ? sb.last_mounted : "(never)");
    printf("  uuid             : ");
    print_uuid(sb.uuid);
    printf("\n");
    printf("  block size       : %u\n", sb.block_size);
    printf("  blocks (total)   : %llu\n", (unsigned long long)sb.blocks_count);
    printf("  blocks (free)    : %llu\n", (unsigned long long)sb.free_blocks_count);
    printf("  blocks per group : %u\n", sb.blocks_per_group);
    printf("  inodes (total)   : %u\n", sb.inodes_count);
    printf("  inodes (free)    : %u\n", sb.free_inodes_count);
    printf("  inodes per group : %u\n", sb.inodes_per_group);
    printf("  inode size       : %u\n", sb.inode_size);
    printf("  size             : %llu MiB\n", (unsigned long long)(total_bytes / (1024u * 1024u)));
    printf("  state            : 0x%04x%s\n", sb.state, (sb.state & 1u) ? " (clean)" : "");
    printf("  rev_level        : %u\n", sb.rev_level);
    printf("  feature_compat   : 0x%08x\n", sb.feature_compat);
    printf("  feature_incompat : 0x%08x\n", sb.feature_incompat);
    printf("  feature_ro_compat: 0x%08x\n", sb.feature_ro_compat);

    return 0;
}
