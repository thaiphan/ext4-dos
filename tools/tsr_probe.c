/* COPY-bug probe.  Tries to open Y:\HELLO.TXT two ways and read from it,
 * reporting exactly which step fails on MS-DOS 4.  No fancy diagnostics —
 * just AH/AL values, error codes, and bytes-read counts.
 *
 * Usage:
 *   ext4prb /e   = ExtOpen (AX=6C00h, mimics COPY) + Read
 *   ext4prb /o   = Regular Open (AH=3Dh) + Read
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dos.h>

static const char path[] = "Y:\\HELLO.TXT";

static int do_extopen(void) {
    union REGS  r;
    struct SREGS s;
    static char buf[512];
    int actual = 0;
    uint16_t handle;

    printf("=== ExtOpen probe ===\n");

    /* INT 21h AX=6C00h Extended Open
     * BX = open mode (0 = read), CX = attribute mask, DX = action,
     * DS:SI = path */
    r.w.ax = 0x6C00u;
    r.w.bx = 0x0000u;        /* read mode */
    r.w.cx = 0x0000u;        /* attribute */
    r.w.dx = 0x0001u;        /* action: open existing */
    segread(&s);
    s.ds   = FP_SEG((void __far *)path);
    r.w.si = FP_OFF((void __far *)path);
    intdosx(&r, &r, &s);
    if (r.x.cflag) {
        printf("  ExtOpen FAILED: AX=%04x\n", r.w.ax);
        return 1;
    }
    handle = r.w.ax;
    printf("  ExtOpen OK: handle=%u, action=%u\n", handle, r.w.cx);

    /* INT 21h AH=3Fh Read
     * BX = handle, CX = bytes, DS:DX = buffer */
    memset(buf, 0, sizeof buf);
    r.w.ax = 0x3F00u;
    r.w.bx = handle;
    r.w.cx = sizeof buf;
    segread(&s);
    s.ds   = FP_SEG((void __far *)buf);
    r.w.dx = FP_OFF((void __far *)buf);
    intdosx(&r, &r, &s);
    if (r.x.cflag) {
        printf("  Read FAILED: AX=%04x\n", r.w.ax);
    } else {
        actual = r.w.ax;
        printf("  Read OK: %d bytes returned\n", actual);
        if (actual > 0) {
            int n = (actual > 50) ? 50 : actual;
            int i;
            printf("  First %d bytes: '", n);
            for (i = 0; i < n; i++) {
                char c = buf[i];
                putchar((c >= 0x20 && c < 0x7F) ? c : '.');
            }
            printf("'\n");
        }
    }

    /* INT 21h AH=3Eh Close */
    r.w.ax = 0x3E00u;
    r.w.bx = handle;
    intdos(&r, &r);
    if (r.x.cflag) {
        printf("  Close FAILED: AX=%04x\n", r.w.ax);
    } else {
        printf("  Close OK\n");
    }
    return actual > 0 ? 0 : 2;
}

static int do_open(void) {
    union REGS  r;
    struct SREGS s;
    static char buf[512];
    int actual = 0;
    uint16_t handle;

    printf("=== Regular Open probe ===\n");

    /* INT 21h AH=3Dh Open Existing
     * AL = open mode (0 = read), DS:DX = path */
    r.w.ax = 0x3D00u;
    segread(&s);
    s.ds   = FP_SEG((void __far *)path);
    r.w.dx = FP_OFF((void __far *)path);
    intdosx(&r, &r, &s);
    if (r.x.cflag) {
        printf("  Open FAILED: AX=%04x\n", r.w.ax);
        return 1;
    }
    handle = r.w.ax;
    printf("  Open OK: handle=%u\n", handle);

    /* Same Read as above */
    memset(buf, 0, sizeof buf);
    r.w.ax = 0x3F00u;
    r.w.bx = handle;
    r.w.cx = sizeof buf;
    segread(&s);
    s.ds   = FP_SEG((void __far *)buf);
    r.w.dx = FP_OFF((void __far *)buf);
    intdosx(&r, &r, &s);
    if (r.x.cflag) {
        printf("  Read FAILED: AX=%04x\n", r.w.ax);
    } else {
        actual = r.w.ax;
        printf("  Read OK: %d bytes returned\n", actual);
        if (actual > 0) {
            int n = (actual > 50) ? 50 : actual;
            int i;
            printf("  First %d bytes: '", n);
            for (i = 0; i < n; i++) {
                char c = buf[i];
                putchar((c >= 0x20 && c < 0x7F) ? c : '.');
            }
            printf("'\n");
        }
    }

    r.w.ax = 0x3E00u;
    r.w.bx = handle;
    intdos(&r, &r);
    if (r.x.cflag) {
        printf("  Close FAILED: AX=%04x\n", r.w.ax);
    } else {
        printf("  Close OK\n");
    }
    return actual > 0 ? 0 : 2;
}

/* Mimic MS-DOS 4 COPY exactly: ExtOpen, then the same intermediate
 * INT 21h calls COPY makes (Get File Times, Get XA Size, IOCTL Get
 * Device Info), THEN try to read.  After each intermediate call,
 * report whether a subsequent Read still works. */
static int do_copy_repro(void) {
    union REGS  r;
    struct SREGS s;
    static char buf[64];
    int actual = 0;
    uint16_t handle;

    printf("=== COPY-flow repro ===\n");

    /* COPY's exact ExtOpen registers (COPY.ASM:595-600):
     *   BX = read_open_mode = 0x0000
     *   CX = 0
     *   DX = read_open_flag = 0x0101  (low=open existing, hi=??? bit 8)
     *   DI = -1                       (no parameter list) */
    r.w.ax = 0x6C00u;
    r.w.bx = 0x0000u;
    r.w.cx = 0x0000u;
    r.w.dx = 0x0101u;
    r.w.di = 0xFFFFu;
    segread(&s);
    s.ds   = FP_SEG((void __far *)path);
    r.w.si = FP_OFF((void __far *)path);
    intdosx(&r, &r, &s);
    if (r.x.cflag) {
        printf("  ExtOpen FAILED: AX=%04x\n", r.w.ax);
        return 1;
    }
    handle = r.w.ax;
    printf("  ExtOpen OK: handle=%u (action=%u)\n", handle, r.w.cx);

    /* COPY step 1: AX=5700h Get File Times (no redirector, kernel-side). */
    r.w.ax = 0x5700u;
    r.w.bx = handle;
    intdos(&r, &r);
    printf("  AX=5700 GetFileTimes: cf=%d AX=%04x CX=%04x DX=%04x\n",
           r.x.cflag, r.w.ax, r.w.cx, r.w.dx);

    /* COPY step 2: AX=5702h Get Extended Attribute Size.
     * BX=handle, SI=querylist or -1, CX=0 means "tell me the size". */
    r.w.ax = 0x5702u;
    r.w.bx = handle;
    r.w.si = 0xFFFFu;
    r.w.cx = 0x0000u;
    intdos(&r, &r);
    printf("  AX=5702 GetXASize : cf=%d AX=%04x CX=%04x\n",
           r.x.cflag, r.w.ax, r.w.cx);

    /* COPY step 3: AX=4400h IOCTL Get Device Info. */
    r.w.ax = 0x4400u;
    r.w.bx = handle;
    intdos(&r, &r);
    printf("  AX=4400 IOCTL    : cf=%d AX=%04x DX=%04x\n",
           r.x.cflag, r.w.ax, r.w.dx);

    /* Now the Read — try a LARGE read like COPY does (BYTCNT can be tens
     * of KB), since maybe our small-CX test wasn't reproducing it. */
    r.w.ax = 0x3F00u;
    r.w.bx = handle;
    r.w.cx = 0x4000u;        /* 16 KB request, like COPY */
    segread(&s);
    s.ds   = FP_SEG((void __far *)buf);
    r.w.dx = FP_OFF((void __far *)buf);
    intdosx(&r, &r, &s);
    if (r.x.cflag) {
        printf("  Read FAILED after intermediates: AX=%04x\n", r.w.ax);
    } else {
        actual = r.w.ax;
        printf("  Read OK after intermediates: %d bytes (asked 16K)\n", actual);
    }

    r.w.ax = 0x3E00u;
    r.w.bx = handle;
    intdos(&r, &r);
    return actual > 0 ? 0 : 2;
}

int main(int argc, char **argv) {
    if (argc >= 2 && (argv[1][0] == '-' || argv[1][0] == '/')) {
        switch (argv[1][1]) {
        case 'e': case 'E': return do_extopen();
        case 'o': case 'O': return do_open();
        case 'c': case 'C': return do_copy_repro();
        }
    }
    /* Default: run all three. */
    do_open();
    printf("\n");
    do_extopen();
    printf("\n");
    do_copy_repro();
    return 0;
}
