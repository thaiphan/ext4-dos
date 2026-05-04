/* ext4mv.exe — INT 21h AH=56h "Rename file" with explicit src + dst paths.
 *
 * Used by the test suite to exercise cross-directory rename through our
 * REM_RENAME bridge.  COMMAND.COM's built-in REN only updates the name
 * (same-dir); to test cross-dir we issue AH=56h directly with both
 * paths fully qualified. */
#include <stdio.h>
#include <dos.h>
#include <i86.h>

int main(int argc, char **argv) {
    union REGS   r;
    struct SREGS s;
    char        *src;
    char        *dst;

    if (argc < 3) {
        printf("usage: ext4mv <src> <dst>\n");
        return 1;
    }
    src = argv[1];
    dst = argv[2];

    /* INT 21h AH=56h: Rename. DS:DX = src, ES:DI = dst. */
    segread(&s);
    r.h.ah = 0x56;
    s.ds   = FP_SEG(src);
    r.x.dx = FP_OFF(src);
    s.es   = FP_SEG(dst);
    r.w.di = FP_OFF(dst);
    r.x.cflag = 0;
    intdosx(&r, &r, &s);
    if (r.x.cflag) {
        printf("Rename '%s' -> '%s' failed: error=0x%02x\n", src, dst, r.w.ax);
        return 1;
    }
    printf("Renamed '%s' -> '%s'\n", src, dst);
    return 0;
}
