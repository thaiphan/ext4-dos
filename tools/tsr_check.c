#include <stdio.h>
#include <dos.h>

#define EXT4_DOS_MAGIC_PROBE 0xE4D0u
#define EXT4_DOS_MAGIC_REPLY 0xE4D5u

extern void debug_int3(void);
#pragma aux debug_int3 = "int 3";

int main(int argc, char **argv) {
    union REGS r;
    int do_break = 0;
    int do_verify = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-' && argv[i][0] != '/') continue;
        switch (argv[i][1]) {
        case 'b': case 'B': do_break  = 1; break;
        case 'v': case 'V': do_verify = 1; break;
        }
    }

    /* Install check. */
    r.w.ax = 0x1100u;
    r.w.bx = EXT4_DOS_MAGIC_PROBE;
    int86(0x2F, &r, &r);
    if (r.h.al == 0xFFu && r.w.bx == EXT4_DOS_MAGIC_REPLY) {
        printf("TSR detected: AL=0x%02x BX=0x%04x (match)\n", r.h.al, r.w.bx);
    } else {
        printf("TSR not detected: AL=0x%02x BX=0x%04x\n", r.h.al, r.w.bx);
    }

    /* Confirm we own all AH=11h subfunctions. */
    r.w.ax = 0x11FEu;
    r.w.bx = 0x1234u;
    r.x.cflag = 0;
    int86(0x2F, &r, &r);
    printf("INT 2Fh AX=0x11FE -> AX=0x%04x CF=%d\n", r.w.ax, r.x.cflag ? 1 : 0);

    if (do_verify) {
        /* /V — compare LIVE g_fs.sb fields to the install-time _DATA
         * snapshots. Disagreement means a stray kernel write has hit
         * g_fs.sb. See "Defending against the residual kernel write" in
         * docs/dos-internals.md. */
        unsigned live_bs, live_bc, live_free;
        unsigned snap_bs, snap_bc, snap_free;
        int ok;

        r.w.ax = 0x11F0u;
        r.w.bx = EXT4_DOS_MAGIC_PROBE;
        int86(0x2F, &r, &r);
        if (r.h.al != 0xFFu) {
            printf("verify: TSR fs not ready\n");
            return 2;
        }
        live_bs = r.w.cx; live_bc = r.w.dx; live_free = r.w.si;

        r.w.ax = 0x11EFu;
        r.w.bx = EXT4_DOS_MAGIC_PROBE;
        int86(0x2F, &r, &r);
        if (r.h.al != 0xFFu) {
            printf("verify: snapshot probe failed\n");
            return 2;
        }
        snap_bs = r.w.cx; snap_bc = r.w.dx; snap_free = r.w.si;

        ok = (live_bs == snap_bs) && (live_bc == snap_bc) && (live_free == snap_free);
        printf("verify: live bs=%04x bc=%04x free=%04x\n", live_bs, live_bc, live_free);
        printf("verify: snap bs=%04x bc=%04x free=%04x -> %s\n",
               snap_bs, snap_bc, snap_free, ok ? "OK" : "CORRUPT");
        if (!ok) return 1;
    }

    if (do_break) {
        printf("Issuing INT 3...\n");
        fflush(stdout);
        debug_int3();
        printf("(resumed)\n");
    }
    return 0;
}
