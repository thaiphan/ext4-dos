/* Write smoke test on Y:\TARGET.TXT:
 *
 *   1. Open r/w (no truncate)
 *   2. Write 1024 bytes of 'B' at offset 0 — in-place overwrite
 *   3. Write 1024 bytes of 'C' at offset 1024 — extend by one block
 *   4. Close
 *
 * After the run TARGET.TXT is 2048 bytes: 1024 'B' + 1024 'C'.
 * Called from run-freedos-test.sh; output to stdout, captured into
 * the OUT.TXT for assertions. */
#include <stdio.h>
#include <string.h>
#include <dos.h>
#include <i86.h>

int main(void) {
    union REGS     r;
    struct SREGS   s;
    static char    path[] = "Y:\\TARGET.TXT";
    /* DGROUP buffer — INT 21h AH=40h reads from DS:DX. */
    static char    wbuf[1024];
    unsigned int   handle;
    unsigned int   written;

    /* INT 21h AH=3Dh AL=2: Open file r/w. */
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

    /* Pass 1: in-place — overwrite the existing 1024 'A' bytes with 'B'. */
    memset(wbuf, 'B', sizeof wbuf);
    segread(&s);
    r.h.ah = 0x40;
    r.w.bx = handle;
    r.w.cx = 1024u;
    r.x.dx = FP_OFF(wbuf);
    s.ds   = FP_SEG(wbuf);
    intdosx(&r, &r, &s);
    if (r.x.cflag) {
        printf("In-place write to Y:\\TARGET.TXT failed: error=0x%02x\n", r.w.ax);
    } else {
        written = r.w.ax;
        printf("In-place wrote %u bytes (B) to Y:\\TARGET.TXT\n", written);
    }

    /* Pass 2: extend — write 1024 'C' bytes at offset 1024. The TSR's
     * REM_WRITE handler routes this through ext4_file_extend_block:
     * allocate a contiguous block, append to the leaf extent, bump the
     * inode size + bitmap + BGD + sb in one journal transaction. */
    memset(wbuf, 'C', sizeof wbuf);
    segread(&s);
    r.h.ah = 0x40;
    r.w.bx = handle;
    r.w.cx = 1024u;
    r.x.dx = FP_OFF(wbuf);
    s.ds   = FP_SEG(wbuf);
    intdosx(&r, &r, &s);
    if (r.x.cflag) {
        printf("Extend write to Y:\\TARGET.TXT failed: error=0x%02x\n", r.w.ax);
    } else {
        written = r.w.ax;
        printf("Extend wrote %u bytes (C) to Y:\\TARGET.TXT\n", written);
    }

    /* INT 21h AH=3Eh: Close. */
    r.h.ah = 0x3E;
    r.w.bx = handle;
    intdos(&r, &r);
    if (r.x.cflag) {
        printf("Close Y:\\TARGET.TXT failed: error=0x%02x\n", r.w.ax);
        return 1;
    }
    return 0;
}
