#include "int13_bdev.h"
#include <stdint.h>
#include <stdlib.h>
#include <i86.h>

#pragma pack(push, 1)
struct dap {
    uint8_t  size;
    uint8_t  reserved;
    uint16_t count;
    uint16_t buf_off;
    uint16_t buf_seg;
    uint32_t lba_lo;
    uint32_t lba_hi;
};
#pragma pack(pop)

extern uint16_t int13_ext_read(uint8_t drive, struct dap *dap);
#pragma aux int13_ext_read = \
    "mov ah, 0x42"           \
    "int 0x13"               \
    "mov ax, 0"              \
    "jnc done"               \
    "mov ax, 1"              \
    "done:"                  \
    parm [dl] [si]           \
    value [ax]               \
    modify [ax];

struct int13_ctx {
    uint8_t drive;
};

static int int13_read(struct blockdev *bd, uint64_t lba, uint32_t count, void *buf) {
    struct int13_ctx *c = (struct int13_ctx *)bd->ctx;
    /* Static so the DAP lives in DGROUP. INT 13h reads DS:SI, and inside an
     * interrupt handler SS != DS — a stack-local DAP would be unreachable
     * via DS-relative addressing. */
    static struct dap dap;
    void __far *fbuf;

    if (count == 0 || count > 127) return BDEV_ERR_RANGE;

    fbuf = (void __far *)buf;

    dap.size     = 16;
    dap.reserved = 0;
    dap.count    = (uint16_t)count;
    dap.buf_off  = FP_OFF(fbuf);
    dap.buf_seg  = FP_SEG(fbuf);
    dap.lba_lo   = (uint32_t)(lba & 0xFFFFFFFFUL);
    dap.lba_hi   = (uint32_t)(lba >> 32);

    if (int13_ext_read(c->drive, &dap) != 0) return BDEV_ERR_IO;
    return BDEV_OK;
}

struct blockdev *int13_bdev_open(uint8_t drive) {
    struct int13_ctx *c;
    struct blockdev *bd;

    c = (struct int13_ctx *)malloc(sizeof *c);
    if (!c) return NULL;
    c->drive = drive;

    bd = (struct blockdev *)malloc(sizeof *bd);
    if (!bd) { free(c); return NULL; }
    bd->read_sectors  = int13_read;
    bd->sector_size   = 512;
    bd->total_sectors = 0;
    bd->ctx           = c;
    return bd;
}

void int13_bdev_close(struct blockdev *bd) {
    if (!bd) return;
    free(bd->ctx);
    free(bd);
}
