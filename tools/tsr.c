#include <stdio.h>
#include <stdlib.h>
#include <dos.h>
#include <i86.h>

#define EXT4_DOS_MAGIC_PROBE 0xE4D0u
#define EXT4_DOS_MAGIC_REPLY 0xE4D5u

static void (__interrupt __far *prev_int2f)(void);

void __interrupt __far my_int2f_handler(union INTPACK r) {
    if (r.w.ax == 0x1100u && r.w.bx == EXT4_DOS_MAGIC_PROBE) {
        r.h.al = 0xFFu;
        r.w.bx = EXT4_DOS_MAGIC_REPLY;
        return;
    }
    _chain_intr(prev_int2f);
}

int main(void) {
    prev_int2f = _dos_getvect(0x2F);
    _dos_setvect(0x2F, my_int2f_handler);

    printf("ext4-dos TSR loaded (probe BX=0x%04x, reply BX=0x%04x)\n",
           EXT4_DOS_MAGIC_PROBE, EXT4_DOS_MAGIC_REPLY);
    fflush(stdout);

    /* Stay resident. 1024 paragraphs = 16 KB — generous for v1; tune later. */
    _dos_keep(0, 1024);
    return 0;
}
