/* Truncate-down smoke test on Y:\RENAMED.TXT:
 *
 *   1. Open r/w
 *   2. Seek to position 100 via INT 21h AH=42h AL=0
 *   3. Write 0 bytes via INT 21h AH=40h CX=0 — DOS convention is "set
 *      EOF to current position", which our REM_WRITE bridge turns into
 *      ext4_file_truncate.
 *   4. Close
 *
 * After the run RENAMED.TXT is 100 bytes (was 2048).  The first 100
 * bytes are preserved (still 'B' from the prior write smoke test). */
#include <stdio.h>
#include <stdlib.h>
#include <dos.h>
#include <i86.h>

int main(int argc, char **argv) {
    union REGS     r;
    struct SREGS   s;
    static char    path_default[] = "Y:\\RENAMED.TXT";
    char          *path = (argc > 1) ? argv[1] : path_default;
    unsigned long  new_size_default = 100ul;
    unsigned long  new_size = (argc > 2) ? (unsigned long)atol(argv[2]) : new_size_default;
    unsigned int   handle;

    /* INT 21h AH=3Dh AL=2: Open r/w. */
    segread(&s);
    r.h.ah = 0x3D;
    r.h.al = 0x02;
    r.x.dx = FP_OFF(path);
    s.ds   = FP_SEG(path);
    intdosx(&r, &r, &s);
    if (r.x.cflag) {
        printf("Open '%s' r/w failed: error=0x%02x\n", path, r.w.ax);
        return 1;
    }
    handle = r.w.ax;

    /* INT 21h AH=42h AL=0: Seek from start. CX:DX = offset. */
    r.h.ah = 0x42;
    r.h.al = 0x00;
    r.w.bx = handle;
    r.w.cx = (unsigned int)(new_size >> 16);
    r.w.dx = (unsigned int)(new_size & 0xFFFFul);
    intdos(&r, &r);
    if (r.x.cflag) {
        printf("Seek '%s' to %lu failed: error=0x%02x\n", path, new_size, r.w.ax);
        return 1;
    }

    /* INT 21h AH=40h CX=0: Write zero bytes => set EOF to current pos. */
    segread(&s);
    r.h.ah = 0x40;
    r.w.bx = handle;
    r.w.cx = 0u;
    r.x.dx = 0u;
    s.ds   = FP_SEG(path);   /* DS:DX value irrelevant when CX=0 */
    intdosx(&r, &r, &s);
    if (r.x.cflag) {
        printf("Truncate '%s' to %lu failed: error=0x%02x\n", path, new_size, r.w.ax);
        r.h.ah = 0x3E; r.w.bx = handle; intdos(&r, &r);
        return 1;
    }
    printf("Truncated '%s' to %lu bytes\n", path, new_size);

    r.h.ah = 0x3E;
    r.w.bx = handle;
    intdos(&r, &r);
    if (r.x.cflag) {
        printf("Close '%s' failed: error=0x%02x\n", path, r.w.ax);
        return 1;
    }
    return 0;
}
