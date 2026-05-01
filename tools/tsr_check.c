#include <stdio.h>
#include <dos.h>

#define EXT4_DOS_MAGIC_PROBE 0xE4D0u
#define EXT4_DOS_MAGIC_REPLY 0xE4D5u

int main(void) {
    union REGS r;
    r.w.ax = 0x1100u;
    r.w.bx = EXT4_DOS_MAGIC_PROBE;
    int86(0x2F, &r, &r);

    if (r.h.al == 0xFFu && r.w.bx == EXT4_DOS_MAGIC_REPLY) {
        printf("TSR detected: AL=0x%02x BX=0x%04x (match)\n", r.h.al, r.w.bx);
        return 0;
    }
    printf("TSR not detected: AL=0x%02x BX=0x%04x\n", r.h.al, r.w.bx);
    return 1;
}
