#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dos.h>
#include <i86.h>

#include "blockdev/blockdev.h"
#include "blockdev/int13_bdev.h"
#include "ext4/superblock.h"
#include "ext4/features.h"
#include "ext4/fs.h"
#include "ext4/inode.h"
#include "ext4/extent.h"
#include "ext4/dir.h"
#include "partition/mbr.h"

#define EXT4_DOS_MAGIC_PROBE 0xE4D0u
#define EXT4_DOS_MAGIC_REPLY 0xE4D5u
#define DRIVE_LETTER 'Y'
#define DRIVE_INDEX  ((DRIVE_LETTER) - 'A')

#define DOS_ERR_FILE_NOT_FOUND  0x02u
#define DOS_ERR_NO_MORE_FILES   0x12u

#define CDS_FLAG_REDIRECTED 0x8000u
#define CDS_ENTRY_SIZE      88
#define CDS_OFF_PATH        0x00
#define CDS_OFF_FLAGS       0x43
#define CDS_OFF_BACKSLASH   0x4F

/* SDA field offsets per FreeDOS kernel.asm.  Should match MS-DOS 4-7. */
#define SDA_PRI_PATH_OFF    0x9E   /* qualified pattern (128 bytes) */
#define SDA_TMP_DM_OFF      0x19E  /* sda_tmp_dm: 21-byte SDB header */
#define SDA_SEARCH_DIR_OFF  0x1B3  /* SearchDir: 32-byte FAT dir entry */
#define SDA_SATTR_OFF       0x24D  /* attribute mask byte */

/* dmatch (sda_tmp_dm) layout — see hdr/dirmatch.h */
#define DM_DRIVE_OFF        0
#define DM_NAME_PAT_OFF     1      /* 11 bytes */
#define DM_ATTR_SRCH_OFF    12
#define DM_ENTRY_OFF        13     /* 2 bytes */

/* struct dirent layout — see hdr/fat.h */
#define DIR_NAME_OFF        0      /* 11 bytes (8.3 padded) */
#define DIR_ATTRIB_OFF      11
#define DIR_TIME_OFF        22
#define DIR_DATE_OFF        24
#define DIR_START_OFF       26
#define DIR_SIZE_OFF        28     /* 4 bytes */

static void (__interrupt __far *prev_int2f)(void);
static char __far *g_cds_entry;
static char __far *g_sda;

static struct blockdev *g_bdev;
static struct ext4_fs   g_fs;
static int              g_fs_ready;
static uint64_t         g_partition_lba;

/* Per-subfunction call counter (indexed by AL & 0x3F).
 * Read back via INT 2Fh AX=11FD, BX=our magic, CL=subfunction; returns
 * count in DX, AL=0xFF on success. Useful for tracing what DOS asks for. */
static uint16_t g_call_counts[64];

/* Diagnostic snapshot of the FIRST FindFirst (AL=1B) call DOS makes us.
 * Probed via INT 2Fh AX=11FC, BX=our magic; reply: AL=0xFF, DX:CX = far
 * pointer to this struct. Used to figure out the SDA/DTA layout DOS uses. */
struct ff_capture {
    uint8_t  valid;
    uint16_t ax, bx, cx, dx;
    uint16_t si, di, bp;
    uint16_t ds, es, flags;
    uint8_t  sda_bytes[256];
    uint8_t  es_di_bytes[64];
    uint8_t  ds_si_bytes[64];
    /* Flow counters for do_findfirst_or_next */
    uint16_t flow_entered;
    uint16_t flow_inode_root_fail;
    int16_t  flow_inode_root_rc;
    uint16_t flow_dir_iter_returned;
    uint16_t flow_state_not_found;
    uint16_t flow_inode_entry_fail;
    uint16_t flow_success;
    /* Snapshot of g_fs key fields at FindFirst time */
    uint8_t  fs_ready_at_call;
    uint32_t fs_bgd_count;
    uint16_t fs_bgd_size;
    uint32_t fs_blocks_per_group;
    uint32_t fs_inodes_per_group;
    uint16_t fs_inode_size;
    uint32_t fs_block_size;
    /* Capture root_inode.i_block (first 16 bytes — extent header) */
    uint8_t  root_iblock[32];
    int16_t  ext_lookup_rc;
    uint32_t ext_lookup_phys_lo;
    uint32_t ext_lookup_phys_hi;
    int16_t  data_bdev_read_rc;
    uint32_t data_sector_lo;
    uint32_t cap_partition_lba_lo;
    uint32_t cap_partition_lba_hi;
    uint32_t cap_byte_off_lo;
    uint32_t cap_byte_off_hi;
    uint16_t cap_sector_size;
};
static struct ff_capture g_ff_capture;

static void to_8_3(const char *name, uint8_t name_len, uint8_t out[11]) {
    int i = 0, j = 0, k;
    for (k = 0; k < 11; k++) out[k] = ' ';
    while (i < name_len && j < 8 && name[i] != '.') {
        uint8_t c = (uint8_t)name[i++];
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 'A');
        out[j++] = c;
    }
    while (i < name_len && name[i] != '.') i++;
    if (i < name_len && name[i] == '.') i++;
    j = 8;
    while (i < name_len && j < 11) {
        uint8_t c = (uint8_t)name[i++];
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 'A');
        out[j++] = c;
    }
}

struct find_iter_state {
    uint16_t target_index;
    uint16_t current_index;
    int      found;
    uint8_t  name_len;
    char     name[256];
    uint32_t inode;
    uint8_t  file_type;
};

static int find_iter_cb(const struct ext4_dir_entry *e, void *ud) {
    struct find_iter_state *s = (struct find_iter_state *)ud;
    /* Skip . and .. — FAT root convention */
    if (e->name_len == 1 && e->name[0] == '.') return 0;
    if (e->name_len == 2 && e->name[0] == '.' && e->name[1] == '.') return 0;

    if (s->current_index == s->target_index) {
        s->name_len  = e->name_len;
        memcpy(s->name, e->name, e->name_len);
        s->name[e->name_len] = '\0';
        s->inode     = e->inode;
        s->file_type = e->file_type;
        s->found     = 1;
        return 1;
    }
    s->current_index++;
    return 0;
}

/* Find the entry at `entry_index` (skipping . / ..) in the ext4 root and
 * fill the SDA SDB + SearchDir buffers. Updates dm_entry to the next index
 * so a subsequent FindNext picks up where this left off. Returns 0 on
 * success, -1 on inode-read failure, -2 on no-such-index. */
static int do_findfirst_or_next(uint16_t entry_index) {
    static struct ext4_inode      root_inode;
    static struct ext4_inode      entry_inode;
    static struct find_iter_state state;
    uint8_t      __far *sdb;
    uint8_t      __far *dirent;
    uint8_t              name83[11];
    int                  i;
    int                  rc;

    g_ff_capture.flow_entered++;
    if (g_ff_capture.flow_entered == 1) {
        g_ff_capture.fs_ready_at_call    = (uint8_t)g_fs_ready;
        g_ff_capture.fs_bgd_count        = g_fs.bgd_count;
        g_ff_capture.fs_bgd_size         = g_fs.bgd_size;
        g_ff_capture.fs_blocks_per_group = g_fs.sb.blocks_per_group;
        g_ff_capture.fs_inodes_per_group = g_fs.sb.inodes_per_group;
        g_ff_capture.fs_inode_size       = g_fs.sb.inode_size;
        g_ff_capture.fs_block_size       = g_fs.sb.block_size;
    }

    rc = ext4_inode_read(&g_fs, 2, &root_inode);
    g_ff_capture.flow_inode_root_rc = (int16_t)rc;
    if (rc != 0) {
        g_ff_capture.flow_inode_root_fail++;
        return -1;
    }
    if (g_ff_capture.flow_entered == 1) {
        for (i = 0; i < 32; i++) g_ff_capture.root_iblock[i] = root_inode.i_block[i];
    }

    state.target_index  = entry_index;
    state.current_index = 0;
    state.found         = 0;
    rc = ext4_dir_iter(&g_fs, &root_inode, find_iter_cb, &state);
    g_ff_capture.flow_dir_iter_returned = (uint16_t)rc;
    if (!state.found) {
        g_ff_capture.flow_state_not_found++;
        return -2;
    }

    if (ext4_inode_read(&g_fs, state.inode, &entry_inode) != 0) {
        g_ff_capture.flow_inode_entry_fail++;
        return -1;
    }

    to_8_3(state.name, state.name_len, name83);

    sdb    = (uint8_t __far *)(g_sda + SDA_TMP_DM_OFF);
    dirent = (uint8_t __far *)(g_sda + SDA_SEARCH_DIR_OFF);

    /* Fill the 32-byte FAT-style dir entry. */
    for (i = 0; i < 32; i++) dirent[i] = 0;
    for (i = 0; i < 11; i++) dirent[DIR_NAME_OFF + i] = name83[i];
    dirent[DIR_ATTRIB_OFF] = (state.file_type == EXT4_FT_DIR) ? 0x10u : 0x00u;
    /* time/date/start_cluster left zero for v1 */
    *(uint32_t __far *)(dirent + DIR_SIZE_OFF) = (uint32_t)entry_inode.size;

    /* Update SDB header. dm_drive needs bit 7 (network) so FreeDOS routes
     * subsequent FindNext via REM_FINDNEXT instead of dos_findnext. */
    sdb[DM_DRIVE_OFF]      = (uint8_t)(DRIVE_INDEX | 0x80u);
    sdb[DM_ATTR_SRCH_OFF]  = g_sda[SDA_SATTR_OFF];
    *(uint16_t __far *)(sdb + DM_ENTRY_OFF) = (uint16_t)(entry_index + 1);

    g_ff_capture.flow_success++;
    return 0;
}

void __interrupt __far my_int2f_handler(union INTPACK r) {
    uint8_t al;

    /* Our install-check protocol */
    if (r.w.ax == 0x1100u && r.w.bx == EXT4_DOS_MAGIC_PROBE) {
        r.h.al = 0xFFu;
        r.w.bx = EXT4_DOS_MAGIC_REPLY;
        return;
    }

    /* Counter-readback debug protocol: AX=11FD, BX=magic, CL=subfn */
    if (r.w.ax == 0x11FDu && r.w.bx == EXT4_DOS_MAGIC_PROBE) {
        r.w.dx = g_call_counts[r.h.cl & 0x3F];
        r.h.al = 0xFFu;
        return;
    }

    /* Capture-pointer probe: AX=11FC, BX=magic; returns DX:CX -> g_ff_capture. */
    if (r.w.ax == 0x11FCu && r.w.bx == EXT4_DOS_MAGIC_PROBE) {
        r.w.dx = FP_SEG((void __far *)&g_ff_capture);
        r.w.cx = FP_OFF((void __far *)&g_ff_capture);
        r.h.al = 0xFFu;
        return;
    }

    if (r.h.ah == 0x11u) {
        al = r.h.al;
        g_call_counts[al & 0x3F]++;

        /* Snapshot the first FindFirst call DOS sends us. */
        if (al == 0x1Bu && !g_ff_capture.valid) {
            uint8_t __far *src;
            int i;

            g_ff_capture.ax    = r.w.ax;
            g_ff_capture.bx    = r.w.bx;
            g_ff_capture.cx    = r.w.cx;
            g_ff_capture.dx    = r.w.dx;
            g_ff_capture.si    = r.w.si;
            g_ff_capture.di    = r.w.di;
            g_ff_capture.bp    = r.x.bp;
            g_ff_capture.ds    = r.x.ds;
            g_ff_capture.es    = r.x.es;
            g_ff_capture.flags = r.w.flags;

            for (i = 0; i < 256; i++) {
                g_ff_capture.sda_bytes[i] = ((uint8_t __far *)g_sda)[i];
            }

            src = (uint8_t __far *)MK_FP(r.x.es, r.w.di);
            for (i = 0; i < 64; i++) g_ff_capture.es_di_bytes[i] = src[i];

            src = (uint8_t __far *)MK_FP(r.x.ds, r.w.si);
            for (i = 0; i < 64; i++) g_ff_capture.ds_si_bytes[i] = src[i];

            g_ff_capture.valid = 1u;
        }

        switch (al) {
        case 0x05: /* ChDir */
        case 0x06: /* Close File */
        case 0x07: /* Commit File */
        case 0x0A: /* Lock Region */
        case 0x0B: /* Unlock Region */
        case 0x0E: /* Set File Attributes */
            r.w.flags &= ~1u;
            return;

        case 0x0C: /* Get Disk Space */
            /* Plausible v1 placeholder: 64 MB total, all free.
             * total = AX * BX * CX = 16384 * 8 * 512 = 64 MiB. */
            r.w.ax = 16384u; /* total clusters */
            r.w.bx = 8u;     /* sectors per cluster */
            r.w.cx = 512u;   /* bytes per sector */
            r.w.dx = 16384u; /* free clusters */
            r.w.flags &= ~1u;
            return;

        case 0x23: /* Qualify Remote File Name */
            /* Pass-through: tell DOS the path it gave us is fine as-is. */
            r.w.flags &= ~1u;
            return;

        case 0x18: /* FindFirst-alt (older DOS) */
        case 0x1B:  /* FindFirst */
            if (!g_fs_ready) {
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }
            if (do_findfirst_or_next(0) != 0) {
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }
            r.w.flags &= ~1u;
            return;

        case 0x1C: { /* FindNext */
            uint16_t entry_index;
            uint8_t __far *sdb;
            if (!g_fs_ready) {
                r.w.ax = DOS_ERR_NO_MORE_FILES;
                r.w.flags |= 1u;
                return;
            }
            sdb = (uint8_t __far *)(g_sda + SDA_TMP_DM_OFF);
            entry_index = *(uint16_t __far *)(sdb + DM_ENTRY_OFF);
            if (do_findfirst_or_next(entry_index) != 0) {
                r.w.ax = DOS_ERR_NO_MORE_FILES;
                r.w.flags |= 1u;
                return;
            }
            r.w.flags &= ~1u;
            return;
        }

        case 0x1D: /* "FindClose" / etc. — be lenient */
            r.w.flags &= ~1u;
            return;

        default:
            r.w.ax = DOS_ERR_FILE_NOT_FOUND;
            r.w.flags |= 1u;
            return;
        }
    }

    _chain_intr(prev_int2f);
}

static char __far *get_lol(void) {
    union REGS  r;
    struct SREGS s;
    r.h.ah = 0x52;
    segread(&s);
    intdosx(&r, &r, &s);
    return (char __far *)MK_FP(s.es, r.w.bx);
}

static char __far *get_sda(void) {
    union REGS  r;
    struct SREGS s;
    r.w.ax = 0x5D06u;
    segread(&s);
    intdosx(&r, &r, &s);
    return (char __far *)MK_FP(s.ds, r.w.si);
}

static int hook_cds(void) {
    char __far *lol;
    char __far *cds_array;
    uint16_t    cds_off, cds_seg;
    uint8_t     lastdrive;
    char __far *cds;
    int         i;

    lol = get_lol();

    cds_off = *(uint16_t __far *)(lol + 0x16);
    cds_seg = *(uint16_t __far *)(lol + 0x18);
    cds_array = (char __far *)MK_FP(cds_seg, cds_off);

    lastdrive = *(uint8_t __far *)(lol + 0x21);

    printf("  LOL    : %04x:%04x\n", FP_SEG(lol), FP_OFF(lol));
    printf("  CDS arr: %04x:%04x\n", cds_seg, cds_off);
    printf("  LASTDRIVE byte at LOL+0x21 = %u\n", (unsigned)lastdrive);

    if ((unsigned)lastdrive <= DRIVE_INDEX) {
        printf("  WARN: LASTDRIVE byte too low; trying %c: anyway\n", DRIVE_LETTER);
    }

    cds = cds_array + (unsigned)DRIVE_INDEX * CDS_ENTRY_SIZE;
    printf("  %c:CDS  : %04x:%04x  (slot %u)\n",
           DRIVE_LETTER, FP_SEG(cds), FP_OFF(cds), DRIVE_INDEX);

    for (i = 0; i < 67; i++) cds[CDS_OFF_PATH + i] = 0;
    cds[CDS_OFF_PATH + 0] = DRIVE_LETTER;
    cds[CDS_OFF_PATH + 1] = ':';
    cds[CDS_OFF_PATH + 2] = '\\';
    cds[CDS_OFF_PATH + 3] = 0;

    *(uint16_t __far *)(cds + CDS_OFF_BACKSLASH) = 2u;
    *(uint16_t __far *)(cds + CDS_OFF_FLAGS) = CDS_FLAG_REDIRECTED;

    g_cds_entry = cds;
    return 0;
}

static int mount_ext4(uint8_t drive) {
    struct mbr_table mbr;
    int rc;
    int i;

    g_bdev = int13_bdev_open(drive);
    if (!g_bdev) {
        printf("  int13_bdev_open(0x%02x) failed\n", drive);
        return -1;
    }

    rc = mbr_read(g_bdev, &mbr);
    g_partition_lba = 0;
    if (rc == 0 && mbr.has_signature) {
        for (i = 0; i < mbr.count; i++) {
            if (mbr.entries[i].type == MBR_TYPE_LINUX) {
                g_partition_lba = mbr.entries[i].start_lba;
                break;
            }
        }
        if (g_partition_lba == 0) {
            printf("  drive 0x%02x has MBR but no Linux partition\n", drive);
            return -2;
        }
    } else {
        printf("  drive 0x%02x: no MBR; treating as bare ext4\n", drive);
    }

    rc = ext4_fs_open(&g_fs, g_bdev, g_partition_lba);
    if (rc != 0) {
        printf("  ext4_fs_open failed (rc=%d)\n", rc);
        return -3;
    }

    g_fs_ready = 1;
    printf("  ext4 mounted: drive 0x%02x, partition LBA %lu\n",
           drive, (unsigned long)g_partition_lba);
    printf("  volume     : %s\n",
           g_fs.sb.volume_name[0] ? g_fs.sb.volume_name : "(unset)");
    printf("  blocks     : %lu (block size %lu)\n",
           (unsigned long)g_fs.sb.blocks_count,
           (unsigned long)g_fs.sb.block_size);
    return 0;
}

int main(int argc, char **argv) {
    uint8_t drive = 0x81;
    char err[100];

    if (argc >= 2) drive = (uint8_t)strtoul(argv[1], NULL, 0);

    if (hook_cds() != 0) {
        return 1;
    }

    g_sda = get_sda();

    printf("ext4-dos TSR\n");
    printf("  drive %c: marked as redirector (flag 0x%04x)\n",
           DRIVE_LETTER, CDS_FLAG_REDIRECTED);
    printf("  SDA at %04x:%04x\n", FP_SEG(g_sda), FP_OFF(g_sda));

    if (mount_ext4(drive) == 0) {
        if (ext4_features_check_supported(&g_fs.sb, err, sizeof err) != 0) {
            printf("  WARN: ext4 mount has unsupported feature: %s\n", err);
            printf("        TSR will respond file-not-found for all entries.\n");
            g_fs_ready = 0;
        }
    } else {
        printf("  WARN: ext4 mount failed; TSR will respond file-not-found.\n");
    }

    prev_int2f = _dos_getvect(0x2F);
    _dos_setvect(0x2F, my_int2f_handler);

    fflush(stdout);

    /* Resident size: generous, covers code + ~16 KB of static buffers. */
    _dos_keep(0, 4096);
    return 0;
}
