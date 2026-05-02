#include "int13_bdev.h"
#include <stddef.h>
#include <stdint.h>
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

/* INT 13h AH=43h — Extended Write. AL=0 means write without read-back
 * verify (faster; modern hardware doesn't need verify). DS:SI points at
 * the same DAP as the read path. */
extern uint16_t int13_ext_write(uint8_t drive, struct dap *dap);
#pragma aux int13_ext_write = \
    "mov ah, 0x43"            \
    "mov al, 0"               \
    "int 0x13"                \
    "mov ax, 0"               \
    "jnc done"                \
    "mov ax, 1"               \
    "done:"                   \
    parm [dl] [si]            \
    value [ax]                \
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

static int int13_write(struct blockdev *bd, uint64_t lba, uint32_t count, const void *buf) {
    struct int13_ctx *c = (struct int13_ctx *)bd->ctx;
    static struct dap dap;
    const void __far *fbuf;

    if (count == 0 || count > 127) return BDEV_ERR_RANGE;

    fbuf = (const void __far *)buf;

    dap.size     = 16;
    dap.reserved = 0;
    dap.count    = (uint16_t)count;
    dap.buf_off  = FP_OFF(fbuf);
    dap.buf_seg  = FP_SEG(fbuf);
    dap.lba_lo   = (uint32_t)(lba & 0xFFFFFFFFUL);
    dap.lba_hi   = (uint32_t)(lba >> 32);

    if (int13_ext_write(c->drive, &dap) != 0) return BDEV_ERR_IO;
    return BDEV_OK;
}

/* Static singletons — TSR resident memory must live in DGROUP, not in
 * the malloc heap. The heap can extend past `_dos_keep`'s paragraph
 * count, in which case DOS reclaims those bytes after we go resident
 * and our blockdev struct gets overwritten (sector_size becomes 0,
 * function pointers garbage). The TSR mounts exactly one ext4 disk,
 * so a single static instance is sufficient. */
static struct int13_ctx g_ctx;
static struct blockdev  g_bd;
static int              g_open;

struct blockdev *int13_bdev_open(uint8_t drive) {
    if (g_open) return NULL;
    g_ctx.drive        = drive;
    g_bd.read_sectors  = int13_read;
    g_bd.write_sectors = int13_write;
    g_bd.sector_size   = 512;
    g_bd.total_sectors = 0;
    g_bd.ctx           = &g_ctx;
    g_open             = 1;
    return &g_bd;
}

void int13_bdev_close(struct blockdev *bd) {
    if (!bd) return;
    g_open = 0;
}
