/* Phase 1b smoke test: writes one 1024-byte block of 'B' over the
 * existing 'A' bytes in Y:\TARGET.TXT.
 *
 *  - Opens for read/write (no truncate)
 *  - Writes exactly 1024 bytes at offset 0
 *  - Closes
 *
 * Called from run-freedos-test.sh. Output goes to stdout for the .bat
 * to capture into OUT.TXT. */
#include <stdio.h>
#include <string.h>
#include <dos.h>
#include <i86.h>

int main(void) {
    union REGS     r;
    struct SREGS   s;
    static char    path[] = "Y:\\TARGET.TXT";
    /* DGROUP buffer — INT 21h AH=40h reads from DS:DX, no SS/DS gymnastics. */
    static char    wbuf[1024];
    unsigned int   handle;
    unsigned int   written;

    memset(wbuf, 'B', sizeof wbuf);

    /* INT 21h AH=3Dh AL=2: Open file r/w. DS:DX = ASCIZ path. */
    segread(&s);
    r.h.ah = 0x3D;
    r.h.al = 0x02;
    r.x.dx = FP_OFF(path);
    s.ds   = FP_SEG(path);
    intdosx(&r, &r, &s);
    if (r.x.cflag) {
        printf("Open Y:\\TARGET.TXT for r/w failed: error=0x%02x\n", r.w.ax);
        return 1;
    }
    handle = r.w.ax;

    /* INT 21h AH=40h: Write to file. BX=handle, CX=count, DS:DX=buffer. */
    segread(&s);
    r.h.ah = 0x40;
    r.w.bx = handle;
    r.w.cx = 1024u;
    r.x.dx = FP_OFF(wbuf);
    s.ds   = FP_SEG(wbuf);
    intdosx(&r, &r, &s);
    if (r.x.cflag) {
        printf("Write to Y:\\TARGET.TXT failed: error=0x%02x\n", r.w.ax);
    } else {
        written = r.w.ax;
        printf("Wrote %u bytes to Y:\\TARGET.TXT\n", written);
    }

    /* INT 21h AH=3Eh: Close. BX=handle. */
    r.h.ah = 0x3E;
    r.w.bx = handle;
    intdos(&r, &r);
    if (r.x.cflag) {
        printf("Close Y:\\TARGET.TXT failed: error=0x%02x\n", r.w.ax);
        return 1;
    }
    return 0;
}
