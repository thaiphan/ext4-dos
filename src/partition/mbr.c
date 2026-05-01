#include "mbr.h"
#include "../blockdev/blockdev.h"
#include "../util/endian.h"
#include <string.h>

int mbr_read(struct blockdev *bd, struct mbr_table *out) {
    uint8_t buf[512];
    int rc;
    int i;

    rc = bdev_read(bd, 0, 1, buf);
    if (rc) return rc;

    memset(out, 0, sizeof *out);

    if (le16(buf + 0x1FE) != 0xAA55u) {
        return -1;
    }
    out->has_signature = 1;

    for (i = 0; i < MBR_MAX_PARTITIONS; i++) {
        const uint8_t *e = buf + 0x1BE + i * 16;
        uint8_t  status = e[0];
        uint8_t  type   = e[4];
        uint32_t start  = le32(e + 8);
        uint32_t count  = le32(e + 12);
        struct mbr_partition *p;
        if (type == 0 || count == 0) continue;
        p = &out->entries[out->count++];
        p->status       = status;
        p->type         = type;
        p->start_lba    = start;
        p->sector_count = count;
        p->bootable     = (status & 0x80u) ? 1 : 0;
    }

    return 0;
}
