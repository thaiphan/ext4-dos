#ifndef PARTITION_MBR_H
#define PARTITION_MBR_H

#include <stdint.h>

#define MBR_MAX_PARTITIONS 4
#define MBR_TYPE_LINUX     0x83u

struct mbr_partition {
    uint8_t  status;
    uint8_t  type;
    uint32_t start_lba;
    uint32_t sector_count;
    int      bootable;
};

struct mbr_table {
    int                  count;
    int                  has_signature;
    struct mbr_partition entries[MBR_MAX_PARTITIONS];
};

struct blockdev;

int mbr_read(struct blockdev *bd, struct mbr_table *out);

#endif
