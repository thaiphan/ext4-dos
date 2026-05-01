#include <stdio.h>
#include <stdint.h>
#include <ctype.h>
#include <dos.h>
#include <i86.h>

#define EXT4_DOS_MAGIC_PROBE 0xE4D0u

struct ff_capture {
    uint8_t  valid;
    uint16_t ax, bx, cx, dx;
    uint16_t si, di, bp;
    uint16_t ds, es, flags;
    uint8_t  sda_bytes[256];
    uint8_t  es_di_bytes[64];
    uint8_t  ds_si_bytes[64];
};

static void hex_dump(const char *label, const uint8_t __far *p, unsigned len) {
    unsigned i, j;
    printf("%s (%u bytes):\n", label, len);
    for (i = 0; i < len; i += 16) {
        printf("  %03x: ", i);
        for (j = 0; j < 16 && i + j < len; j++) {
            printf("%02x ", p[i + j]);
        }
        for (; j < 16; j++) printf("   ");
        printf(" |");
        for (j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = p[i + j];
            putchar(isprint(c) ? (char)c : '.');
        }
        printf("|\n");
    }
}

int main(void) {
    union REGS r;
    struct ff_capture __far *cap;

    r.w.ax = 0x11FCu;
    r.w.bx = EXT4_DOS_MAGIC_PROBE;
    int86(0x2F, &r, &r);
    if (r.h.al != 0xFFu) {
        printf("capture probe failed: AL=0x%02x\n", r.h.al);
        return 1;
    }

    cap = (struct ff_capture __far *)MK_FP(r.w.dx, r.w.cx);
    printf("capture buffer at %04x:%04x\n", r.w.dx, r.w.cx);

    if (!cap->valid) {
        printf("no FindFirst captured yet (valid=0)\n");
        return 0;
    }

    printf("FindFirst entry registers:\n");
    printf("  AX=%04x BX=%04x CX=%04x DX=%04x  flags=%04x\n",
           cap->ax, cap->bx, cap->cx, cap->dx, cap->flags);
    printf("  SI=%04x DI=%04x BP=%04x  DS=%04x ES=%04x\n",
           cap->si, cap->di, cap->bp, cap->ds, cap->es);

    hex_dump("SDA[0..255]", cap->sda_bytes, 256);
    hex_dump("ES:DI[0..63] (likely DTA)", cap->es_di_bytes, 64);
    hex_dump("DS:SI[0..63] (likely pattern)", cap->ds_si_bytes, 64);

    return 0;
}
