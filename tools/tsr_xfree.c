/* ext4xfr.exe — exercise INT 2Fh AX=11A3h (REM_GETLARGESPACE) and
 * INT 21h AX=7303h (Get Extended Free Space).
 *
 *   AX=11A3h is dispatched directly to our redirector and bypasses the
 *   kernel, so it works on every DOS that loads our TSR.
 *
 *   AX=7303h goes through the kernel.  FreeDOS Build 2045+ routes it to
 *   AX=11A3h for redirector drives; older FreeDOS builds fall back to
 *   the legacy AL=0Ch path; MS-DOS 4 returns "invalid function" since
 *   its kernel doesn't know AX=7303h at all.
 *
 * Layout of the xfreespace struct matches FreeDOS hdr/xstructs.h. */

#include <stdio.h>
#include <string.h>
#include <dos.h>

#pragma pack(push, 1)
struct xfreespace {
    unsigned short xfs_datasize;
    unsigned short xfs_version;
    unsigned long  xfs_clussize;       /* sectors per cluster */
    unsigned long  xfs_secsize;        /* bytes per sector */
    unsigned long  xfs_freeclusters;
    unsigned long  xfs_totalclusters;
    unsigned long  xfs_freesectors;
    unsigned long  xfs_totalsectors;
    unsigned long  xfs_freeunits;
    unsigned long  xfs_totalunits;
    unsigned char  xfs_reserved[8];
};
#pragma pack(pop)

int main(int argc, char **argv) {
    static struct xfreespace xfs;
    static const char drive_default[] = "Y:\\";
    const char *drive = drive_default;
    union REGS  r;
    struct SREGS s;
    unsigned long total_blocks, free_blocks;
    unsigned long bytes_total, bytes_free;

    if (argc > 1) drive = argv[1];

    /* --- Direct INT 2Fh AX=11A3h (REM_GETLARGESPACE) ------------------ */
    /* Our redirector handler doesn't validate ES:DI, so we just pass the
     * xfs buffer (any FAR pointer works). On return:
     *   AX=total_hi BX=total_lo CX=free_hi DX=free_lo SI=bytes/sector
     *   CF=0 on success, CF=1 if the redirector isn't ours / not ready. */
    segread(&s);
    r.w.ax = 0x11A3u;
    s.es   = FP_SEG(&xfs);
    r.w.di = FP_OFF(&xfs);
    r.x.cflag = 0;
    int86x(0x2F, &r, &r, &s);

    if (r.x.cflag) {
        printf("INT 2Fh AX=11A3h failed: AX=0x%04x CF=1\n", r.w.ax);
        printf("  (no ext4-dos TSR loaded? or g_fs not ready)\n");
        return 2;
    }

    total_blocks = ((unsigned long)r.w.ax << 16) | r.w.bx;
    free_blocks  = ((unsigned long)r.w.cx << 16) | r.w.dx;
    bytes_total  = total_blocks * (unsigned long)r.w.si;
    bytes_free   = free_blocks  * (unsigned long)r.w.si;
    printf("INT 2Fh AX=11A3h direct (bypass kernel):\n");
    printf("  total blocks    : %lu\n", total_blocks);
    printf("  free  blocks    : %lu\n", free_blocks);
    printf("  bytes/block     : %u\n",  r.w.si);
    printf("  bytes total     : %lu\n", bytes_total);
    printf("  bytes free      : %lu\n", bytes_free);

    /* --- INT 21h AX=7303h via kernel ---------------------------------- */
    memset(&xfs, 0, sizeof xfs);
    xfs.xfs_datasize = sizeof xfs;

    segread(&s);
    r.w.ax = 0x7303u;
    r.w.cx = sizeof xfs;
    s.ds   = FP_SEG(drive);
    r.w.dx = FP_OFF(drive);
    s.es   = FP_SEG(&xfs);
    r.w.di = FP_OFF(&xfs);
    r.x.cflag = 0;
    intdosx(&r, &r, &s);

    printf("INT 21h AX=7303h on '%s' (via kernel):\n", drive);
    if (r.x.cflag) {
        printf("  failed: AX=0x%04x CF=1 (kernel doesn't support AH=73h)\n", r.w.ax);
        return 0;
    }

    printf("  bytes/sector    : %lu\n", xfs.xfs_secsize);
    printf("  sectors/cluster : %lu\n", xfs.xfs_clussize);
    printf("  total clusters  : %lu\n", xfs.xfs_totalclusters);
    printf("  free  clusters  : %lu\n", xfs.xfs_freeclusters);
    bytes_total = xfs.xfs_totalclusters * xfs.xfs_clussize * xfs.xfs_secsize;
    bytes_free  = xfs.xfs_freeclusters  * xfs.xfs_clussize * xfs.xfs_secsize;
    printf("  bytes total     : %lu\n", bytes_total);
    printf("  bytes free      : %lu\n", bytes_free);

    return 0;
}
