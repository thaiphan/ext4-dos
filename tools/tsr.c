#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dos.h>
#include <i86.h>

#define EXT4_DOS_MAGIC_PROBE 0xE4D0u
#define EXT4_DOS_MAGIC_REPLY 0xE4D5u
#define DRIVE_LETTER 'Y'
#define DRIVE_INDEX  ((DRIVE_LETTER) - 'A')

#define DOS_ERR_FILE_NOT_FOUND  0x02

#define CDS_FLAG_REDIRECTED 0x8000u
#define CDS_ENTRY_SIZE      88
#define CDS_OFF_PATH        0x00
#define CDS_OFF_FLAGS       0x43
#define CDS_OFF_BACKSLASH   0x4F

static void (__interrupt __far *prev_int2f)(void);
static char __far *g_cds_entry;
static char __far *g_sda;

void __interrupt __far my_int2f_handler(union INTPACK r) {
    if (r.w.ax == 0x1100u && r.w.bx == EXT4_DOS_MAGIC_PROBE) {
        r.h.al = 0xFFu;
        r.w.bx = EXT4_DOS_MAGIC_REPLY;
        return;
    }

    /* Redirector multiplex AH=11h: claim and return file-not-found */
    if (r.h.ah == 0x11u) {
        r.w.ax = DOS_ERR_FILE_NOT_FOUND;
        r.w.flags |= 1u;
        return;
    }

    _chain_intr(prev_int2f);
}

static char __far *get_lol(void) {
    union REGS  r;
    struct SREGS s;
    r.h.ah = 0x52;
    segread(&s);
    intdosx(&r, &r, &s);
    return (char __far *)MK_FP(s.es, r.w.bx);
}

static char __far *get_sda(void) {
    union REGS  r;
    struct SREGS s;
    r.w.ax = 0x5D06u;
    segread(&s);
    intdosx(&r, &r, &s);
    return (char __far *)MK_FP(s.ds, r.w.si);
}

static int hook_cds(void) {
    char __far *lol;
    char __far *cds_array;
    uint16_t    cds_off, cds_seg;
    uint8_t     lastdrive;
    char __far *cds;
    int         i;

    lol = get_lol();

    cds_off = *(uint16_t __far *)(lol + 0x16);
    cds_seg = *(uint16_t __far *)(lol + 0x18);
    cds_array = (char __far *)MK_FP(cds_seg, cds_off);

    lastdrive = *(uint8_t __far *)(lol + 0x21);

    printf("  LOL    : %04x:%04x\n", FP_SEG(lol), FP_OFF(lol));
    printf("  CDS arr: %04x:%04x\n", cds_seg, cds_off);
    printf("  LASTDRIVE byte at LOL+0x21 = %u\n", (unsigned)lastdrive);

    if ((unsigned)lastdrive <= DRIVE_INDEX) {
        printf("  WARN: LASTDRIVE byte too low; trying %c: anyway\n", DRIVE_LETTER);
    }

    cds = cds_array + (unsigned)DRIVE_INDEX * CDS_ENTRY_SIZE;
    printf("  %c:CDS  : %04x:%04x  (slot %u)\n",
           DRIVE_LETTER, FP_SEG(cds), FP_OFF(cds), DRIVE_INDEX);

    for (i = 0; i < 67; i++) cds[CDS_OFF_PATH + i] = 0;
    cds[CDS_OFF_PATH + 0] = DRIVE_LETTER;
    cds[CDS_OFF_PATH + 1] = ':';
    cds[CDS_OFF_PATH + 2] = '\\';
    cds[CDS_OFF_PATH + 3] = 0;

    *(uint16_t __far *)(cds + CDS_OFF_BACKSLASH) = 2u;
    *(uint16_t __far *)(cds + CDS_OFF_FLAGS) = CDS_FLAG_REDIRECTED;

    g_cds_entry = cds;
    return 0;
}

int main(void) {
    if (hook_cds() != 0) {
        return 1;
    }

    g_sda = get_sda();

    prev_int2f = _dos_getvect(0x2F);
    _dos_setvect(0x2F, my_int2f_handler);

    printf("ext4-dos TSR loaded\n");
    printf("  drive %c: marked as redirector (flag 0x%04x)\n",
           DRIVE_LETTER, CDS_FLAG_REDIRECTED);
    printf("  SDA at %04x:%04x\n", FP_SEG(g_sda), FP_OFF(g_sda));
    fflush(stdout);

    _dos_keep(0, 1024);
    return 0;
}
