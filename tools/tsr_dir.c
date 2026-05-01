#include <stdio.h>
#include <string.h>
#include <dos.h>
#include <i86.h>

int main(void) {
    union REGS  r;
    struct SREGS s;
    char dta[128];
    static char pattern[] = "Y:\\*.*";

    /* INT 21h AH=1A: Set DTA. DS:DX -> our buffer. */
    segread(&s);
    r.h.ah = 0x1A;
    r.x.dx = FP_OFF(dta);
    intdosx(&r, &r, &s);

    /* INT 21h AH=4E: FindFirst. DS:DX -> ASCIZ pattern, CX = attr mask. */
    r.h.ah = 0x4E;
    r.w.cx = 0x37u;
    r.x.dx = FP_OFF(pattern);
    s.ds   = FP_SEG(pattern);
    intdosx(&r, &r, &s);

    if (r.x.cflag) {
        printf("FindFirst Y:\\*.* error=0x%02x\n", r.w.ax);
        return 1;
    }
    printf("FindFirst Y:\\*.* succeeded (unexpected for stub TSR)\n");
    return 0;
}
