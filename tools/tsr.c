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

/* Both bits must be set for MS-DOS 4 to dispatch file ops through INT 2Fh
 * AH=11h on this drive. 0x8000 = curdir_isnet (network/IFS drive), 0x4000 =
 * curdir_inuse (slot is live). FreeDOS works with just 0x8000 but MS-DOS 4
 * silently treats isnet-without-inuse as "drive not present", so DIR Y:
 * etc. fall back to the local FAT path and the kernel never calls our
 * redirector. See references/msdos4/v4.0/src/CMD/IFSFUNC/IFSSESS.ASM
 * (`MOV [SI.curdir_flags], curdir_isnet + curdir_inuse`) and
 * references/msdos4/v4.0/src/INC/CURDIR.INC for the bit definitions. */
#define CDS_FLAG_REDIRECTED 0xC000u
#define CDS_ENTRY_SIZE      88
#define CDS_OFF_PATH        0x00
#define CDS_OFF_FLAGS       0x43
#define CDS_OFF_BACKSLASH   0x4F

/* SDA field offsets per FreeDOS kernel.asm.  Should match MS-DOS 4-7. */
#define SDA_DTA_OFF         0x0C   /* DWORD far ptr to user's DTA / Read buffer */
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

/* SFT offsets (DOS 4+) per RBIL */
#define SFT_REF_COUNT_OFF       0x00  /* word: number of handles referencing */
#define SFT_OPEN_MODE_OFF       0x02  /* word: open mode (low byte = access) */
#define SFT_FILE_ATTR_OFF       0x04  /* byte */
#define SFT_DEVINFO_OFF         0x05  /* word: bit 15 = network */
#define SFT_FILE_TIME_OFF       0x0D  /* word */
#define SFT_FILE_DATE_OFF       0x0F  /* word */
#define SFT_FILE_SIZE_OFF       0x11  /* dword */
#define SFT_FILE_POSITION_OFF   0x15  /* dword */
#define SFT_FILE_NAME_OFF       0x20  /* 11 bytes 8.3 */

static void (__interrupt __far *prev_int2f)(void);
static char __far *g_cds_entry;
static char __far *g_sda;

static struct blockdev *g_bdev;
static struct ext4_fs   g_fs;
static int              g_fs_ready;
static uint64_t         g_partition_lba;
static int              g_quiet;  /* -q: suppress install banner (for CONFIG.SYS INSTALL=) */

/* Open-file slot table.
 *
 * DOS passes us the SFT (system file table entry) pointer in ES:DI on every
 * Open / Read / Close call. The SFT is the kernel's per-handle bookkeeping
 * struct — it stays at the same address for the lifetime of an open file
 * (a different file uses a different SFT entry). We use the (seg, off) of
 * the SFT as our key into this slot table.
 *
 * 8 slots is plenty for typical DOS workloads: COPY uses 2, a compiler
 * compiling a single file with a couple of #includes uses ~5, etc. The
 * default DOS FILES=8 limit is the same.
 *
 * Lookup is O(N) linear search — at N=8 the search is faster than a hash
 * computation, and it keeps the code straightforward in an interrupt
 * context where SS!=DS pitfalls are easy to fall into. */
#define MAX_OPEN_SLOTS 8

static struct open_slot {
    uint8_t            used;
    uint16_t           sft_seg;     /* identity: matches caller's ES on Read/Close */
    uint16_t           sft_off;     /*           matches caller's DI on Read/Close */
    uint32_t           inode_num;
    struct ext4_inode  inode;
} g_open[MAX_OPEN_SLOTS];

/* Find the slot whose SFT pointer matches (sft_seg:sft_off). Returns
 * a pointer into g_open, or NULL. */
static struct open_slot *find_open_slot(uint16_t sft_seg, uint16_t sft_off) {
    int i;
    for (i = 0; i < MAX_OPEN_SLOTS; i++) {
        if (g_open[i].used &&
            g_open[i].sft_seg == sft_seg &&
            g_open[i].sft_off == sft_off) {
            return &g_open[i];
        }
    }
    return (struct open_slot *)0;
}

/* Allocate a fresh slot for (sft_seg:sft_off). If a slot is already
 * associated with that SFT pointer (stale state from a previous open
 * we never saw closed), reuse it. Otherwise pick the first free slot. */
static struct open_slot *alloc_open_slot(uint16_t sft_seg, uint16_t sft_off) {
    int i;
    struct open_slot *existing = find_open_slot(sft_seg, sft_off);
    if (existing) return existing;
    for (i = 0; i < MAX_OPEN_SLOTS; i++) {
        if (!g_open[i].used) return &g_open[i];
    }
    return (struct open_slot *)0;
}

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
    /* Per-call snapshot of the first 4 FindFirst calls (packed for
     * cross-translation-unit layout consistency with tsr_dump.exe). */
#pragma pack(push, 1)
    struct {
        uint8_t  used;
        uint8_t  sattr;
        int16_t  rc;
        uint16_t target_index;
        uint16_t current_index_after;
        uint8_t  state_found;
        uint8_t  name_len;
        char     name[16];
        uint8_t  pri_path[80];
        uint8_t  name83[11];
        uint8_t  searchdir_after[32];
    } calls[4];
#pragma pack(pop)
    /* Open/Read/Close diagnostics */
    uint16_t open_call_count;
    uint16_t read_call_count;
    uint16_t close_call_count;
    int16_t  last_open_rc;
    uint8_t  last_open_path[64];
    uint32_t last_open_inode_num;
    uint32_t last_open_size;
    uint32_t last_read_pos;
    uint16_t last_read_count;
    int16_t  last_read_actual;
    uint32_t entry_inode_mtime;
    uint16_t entry_inode_dos_time;
    uint16_t entry_inode_dos_date;
    uint32_t utd_initial_days;
    uint16_t utd_year_iters;
    uint32_t utd_days_after_year_loop;
    uint16_t utd_final_year;
};
static struct ff_capture g_ff_capture;

static int is_leap_year(int year) {
    if (year % 400 == 0) return 1;
    if (year % 100 == 0) return 0;
    if (year %   4 == 0) return 1;
    return 0;
}

/* Convert Unix seconds-since-1970 (UTC) to packed DOS time/date.
 * Returns date<<16 | time (both 16 bits). Returning a value avoids the
 * SS!=DS hazard of writing through caller-supplied pointers in interrupt
 * context — caller would otherwise pass &stack_local. */
static uint32_t unix_to_dos(uint32_t unix_secs) {
    uint16_t dos_time_local, dos_date_local;
    uint16_t *dos_time = &dos_time_local;
    uint16_t *dos_date = &dos_date_local;
    static const uint8_t dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint32_t days = unix_secs / 86400ul;
    uint32_t s    = unix_secs % 86400ul;
    int      year  = 1970;
    int      month = 1;
    int      day;
    int      yd, md;
    uint8_t  hour, mn, sc;
    uint16_t iters = 0;

    g_ff_capture.utd_initial_days = days;

    while (1) {
        yd = is_leap_year(year) ? 366 : 365;
        if (days < (uint32_t)yd) break;
        days -= (uint32_t)yd;
        year++;
        iters++;
        if (iters > 200) break;  /* safety */
    }
    g_ff_capture.utd_year_iters = iters;
    g_ff_capture.utd_days_after_year_loop = days;
    g_ff_capture.utd_final_year = (uint16_t)year;
    while (1) {
        md = (int)dim[month - 1];
        if (month == 2 && is_leap_year(year)) md = 29;
        if (days < (uint32_t)md) break;
        days -= (uint32_t)md;
        month++;
    }
    day = (int)days + 1;

    hour = (uint8_t)(s / 3600u);
    mn   = (uint8_t)((s / 60u) % 60u);
    sc   = (uint8_t)(s % 60u);

    if (year < 1980) {
        *dos_date = 0x0021u; /* 1980-01-01 — DOS minimum */
    } else {
        *dos_date = (uint16_t)((((uint16_t)(year - 1980)) << 9)
                               | (((uint16_t)month) << 5)
                               | (uint16_t)day);
    }
    *dos_time = (uint16_t)(((uint16_t)hour << 11)
                           | ((uint16_t)mn << 5)
                           | (uint16_t)(sc / 2u));
    return ((uint32_t)*dos_date << 16) | (uint32_t)*dos_time;
}

/* Convert DOS path "Y:\HELLO.TXT" (qualified, in SDA+0x9E) to lowercase
 * ext4 path "/hello.txt". Returns 0 on success, -1 on bad path. */
static int dos_to_ext4_path(char *out, int out_size) {
    const char __far *src = (const char __far *)(g_sda + SDA_PRI_PATH_OFF);
    int j;
    if (src[0] != DRIVE_LETTER || src[1] != ':') return -1;
    src += 2;
    if (out_size < 2) return -1;

    out[0] = '/';
    j = 1;
    if (*src == '\\' || *src == '/') src++;

    while (*src && j < out_size - 1) {
        char c = *src;
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[j++] = c;
        src++;
    }
    out[j] = 0;
    return 0;
}

/* Read up to `count` bytes from `inode` starting at byte `offset` into
 * `out` (far pointer, may be in caller's segment). Returns bytes copied. */
static int read_file_bytes(const struct ext4_inode *inode, uint32_t offset,
                           uint16_t count, uint8_t __far *out) {
    static uint8_t blk[4096];
    uint32_t bs = g_fs.sb.block_size;
    uint32_t bytes_done = 0;
    uint32_t cur, logical_block, in_block, can_take, i;

    if (bs > sizeof blk) return 0;

    while (bytes_done < (uint32_t)count) {
        cur = offset + bytes_done;
        if (cur >= (uint32_t)inode->size) break;
        logical_block = cur / bs;
        in_block = cur - logical_block * bs;
        can_take = bs - in_block;
        if (can_take > (uint32_t)count - bytes_done)
            can_take = (uint32_t)count - bytes_done;
        if (cur + can_take > (uint32_t)inode->size)
            can_take = (uint32_t)inode->size - cur;

        if (ext4_file_read_block(&g_fs, inode, logical_block, blk) != 0) break;

        for (i = 0; i < can_take; i++) {
            out[bytes_done + i] = blk[in_block + i];
        }
        bytes_done += can_take;
    }

    return (int)bytes_done;
}

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

static int name_is_8_3_safe(const char *name, uint8_t name_len) {
    /* DOS 8.3 char set: A-Z 0-9 ! # $ % & ' ( ) - @ ^ _ ` { } ~ + spaces.
     * We're conservative: alnum + a few common safe chars. Reject names
     * whose base or extension don't fit 8.3 too. */
    int i;
    int dot_at = -1;
    for (i = 0; i < name_len; i++) {
        char c = name[i];
        if (c == '.') {
            if (dot_at >= 0) return 0;  /* multiple dots */
            dot_at = i;
            continue;
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~')) {
            return 0;
        }
    }
    if (dot_at < 0) {
        if (name_len > 8) return 0;
    } else {
        if (dot_at > 8) return 0;
        if (name_len - dot_at - 1 > 3) return 0;
    }
    return 1;
}

static int find_iter_cb(const struct ext4_dir_entry *e, void *ud) {
    struct find_iter_state *s = (struct find_iter_state *)ud;
    /* Skip . and .. — FAT root convention */
    if (e->name_len == 1 && e->name[0] == '.') return 0;
    if (e->name_len == 2 && e->name[0] == '.' && e->name[1] == '.') return 0;
    /* Skip names that don't fit DOS 8.3 (no LFN support yet). */
    if (!name_is_8_3_safe(e->name, e->name_len)) return 0;

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
static uint8_t g_ff_call_idx = 0;

static int do_findfirst_or_next(uint16_t entry_index) {
    static struct ext4_inode      root_inode;
    static struct ext4_inode      entry_inode;
    static struct find_iter_state state;
    uint8_t      __far *sdb;
    uint8_t      __far *dirent;
    /* Static so &name83 resolves via DS, not SS. to_8_3() writes to it
     * via near pointer, and in interrupt context SS != DS. */
    static uint8_t       name83[11];
    int                  i;
    int                  rc;
    int                  result = 0;
    int                  slot;

    slot = (g_ff_call_idx < 4) ? (int)g_ff_call_idx : -1;
    g_ff_call_idx++;
    if (slot >= 0) {
        g_ff_capture.calls[slot].used = 1;
        g_ff_capture.calls[slot].target_index = entry_index;
        g_ff_capture.calls[slot].sattr = g_sda[SDA_SATTR_OFF];
        for (i = 0; i < 80; i++)
            g_ff_capture.calls[slot].pri_path[i] = g_sda[SDA_PRI_PATH_OFF + i];
    }

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
        result = -1;
        goto done;
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
        result = -2;
        goto done;
    }

    if (ext4_inode_read(&g_fs, state.inode, &entry_inode) != 0) {
        g_ff_capture.flow_inode_entry_fail++;
        result = -1;
        goto done;
    }

    to_8_3(state.name, state.name_len, name83);

    sdb    = (uint8_t __far *)(g_sda + SDA_TMP_DM_OFF);
    dirent = (uint8_t __far *)(g_sda + SDA_SEARCH_DIR_OFF);

    /* Fill the 32-byte FAT-style dir entry. */
    for (i = 0; i < 32; i++) dirent[i] = 0;
    for (i = 0; i < 11; i++) dirent[DIR_NAME_OFF + i] = name83[i];
    dirent[DIR_ATTRIB_OFF] = (state.file_type == EXT4_FT_DIR) ? 0x10u : 0x00u;
    {
        uint32_t td = unix_to_dos(entry_inode.mtime);
        uint16_t dt = (uint16_t)(td & 0xFFFFul);
        uint16_t dd = (uint16_t)(td >> 16);
        *(uint16_t __far *)(dirent + DIR_TIME_OFF) = dt;
        *(uint16_t __far *)(dirent + DIR_DATE_OFF) = dd;
        if (slot == 0) {
            g_ff_capture.entry_inode_mtime    = entry_inode.mtime;
            g_ff_capture.entry_inode_dos_time = dt;
            g_ff_capture.entry_inode_dos_date = dd;
        }
    }
    *(uint32_t __far *)(dirent + DIR_SIZE_OFF) = (uint32_t)entry_inode.size;

    /* Update SDB header. dm_drive needs bit 7 (network) so FreeDOS routes
     * subsequent FindNext via REM_FINDNEXT instead of dos_findnext. */
    sdb[DM_DRIVE_OFF]      = (uint8_t)(DRIVE_INDEX | 0x80u);
    sdb[DM_ATTR_SRCH_OFF]  = g_sda[SDA_SATTR_OFF];
    *(uint16_t __far *)(sdb + DM_ENTRY_OFF) = (uint16_t)(entry_index + 1);

    if (slot >= 0) {
        for (i = 0; i < 11; i++) g_ff_capture.calls[slot].name83[i] = name83[i];
        for (i = 0; i < 32; i++)
            g_ff_capture.calls[slot].searchdir_after[i] = dirent[i];
    }

    g_ff_capture.flow_success++;
    result = 0;

done:
    if (slot >= 0) {
        g_ff_capture.calls[slot].rc = (int16_t)result;
        g_ff_capture.calls[slot].current_index_after = state.current_index;
        g_ff_capture.calls[slot].state_found = (uint8_t)state.found;
        if (state.found) {
            uint8_t copy_len = (state.name_len < 15u) ? state.name_len : 15u;
            for (i = 0; i < copy_len; i++)
                g_ff_capture.calls[slot].name[i] = state.name[i];
            g_ff_capture.calls[slot].name[copy_len] = '\0';
            g_ff_capture.calls[slot].name_len = state.name_len;
        }
    }
    return result;
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
        case 0x07: /* Commit File */
        case 0x0A: /* Lock Region */
        case 0x0B: /* Unlock Region */
        case 0x0E: /* Set File Attributes */
            r.w.flags &= ~1u;
            return;

        case 0x06: { /* Close File */
            struct open_slot *slot;
            g_ff_capture.close_call_count++;
            slot = find_open_slot(r.x.es, r.w.di);
            if (slot) slot->used = 0;
            /* If no slot matched, the SFT wasn't ours — be lenient and
             * return success anyway. DOS may close handles in bulk
             * during process termination. */
            r.w.flags &= ~1u;
            return;
        }

        case 0x15: /* MS-DOS 4 IFS_OPEN — handle-based open */
        case 0x2E: /* MS-DOS 4 Extended Open (issued from FT.IFS_extopen path) */
        case 0x16: { /* Open Existing File (IFS_SEQ_OPEN) */
            static char path_buf[128];
            uint8_t __far *sft;
            uint32_t inode_num;
            int rc_path;
            int dbg_i;
            struct open_slot *slot;

            g_ff_capture.open_call_count++;
            if (!g_fs_ready) {
                g_ff_capture.last_open_rc = -100;
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }
            rc_path = dos_to_ext4_path(path_buf, sizeof path_buf);
            for (dbg_i = 0; dbg_i < 64; dbg_i++)
                g_ff_capture.last_open_path[dbg_i] = (uint8_t)path_buf[dbg_i];
            if (rc_path != 0) {
                g_ff_capture.last_open_rc = -101;
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }
            inode_num = ext4_path_lookup(&g_fs, path_buf);
            g_ff_capture.last_open_inode_num = inode_num;
            if (inode_num == 0) {
                g_ff_capture.last_open_rc = -102;
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }

            slot = alloc_open_slot(r.x.es, r.w.di);
            if (!slot) {
                /* All MAX_OPEN_SLOTS in use. Return "too many open files"
                 * (DOS error 4) so callers see a sensible error. */
                g_ff_capture.last_open_rc = -104;
                r.w.ax = 4u;            /* ERROR_TOO_MANY_OPEN_FILES */
                r.w.flags |= 1u;
                return;
            }
            if (ext4_inode_read(&g_fs, inode_num, &slot->inode) != 0) {
                g_ff_capture.last_open_rc = -103;
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }
            slot->used      = 1;
            slot->sft_seg   = r.x.es;
            slot->sft_off   = r.w.di;
            slot->inode_num = inode_num;
            g_ff_capture.last_open_size = (uint32_t)slot->inode.size;
            g_ff_capture.last_open_rc = 0;

            sft = (uint8_t __far *)MK_FP(r.x.es, r.w.di);
            /* sf_ref_count must be 1 — kernel checks before treating the
             * SFT as live. MS-DOS 4 doesn't pre-set this for ExtOpen
             * (AL=0x2E) and the SFT we receive may have stale ref_count
             * from a previous slot user, so DOS treats the open as
             * invalid and returns "Invalid function" to the caller. */
            *(uint16_t __far *)(sft + SFT_REF_COUNT_OFF) = 1u;
            /* sf_open_mode: keep whatever the kernel pre-set in the low
             * byte (read/write/share bits) but make sure the high byte is
             * clean. Kernel pre-set the low byte just before our call. */
            *(uint16_t __far *)(sft + SFT_DEVINFO_OFF) =
                (uint16_t)(0x8000u | DRIVE_INDEX);
            sft[SFT_FILE_ATTR_OFF] = 0;
            {
                uint32_t td = unix_to_dos(slot->inode.mtime);
                *(uint16_t __far *)(sft + SFT_FILE_TIME_OFF) =
                    (uint16_t)(td & 0xFFFFul);
                *(uint16_t __far *)(sft + SFT_FILE_DATE_OFF) =
                    (uint16_t)(td >> 16);
            }
            *(uint32_t __far *)(sft + SFT_FILE_SIZE_OFF) =
                (uint32_t)slot->inode.size;
            *(uint32_t __far *)(sft + SFT_FILE_POSITION_OFF) = 0;

            /* For AX=6C00h Extended Open dispatch (AL=0x2E), DOS expects
             * CX = "action taken" on success (1 = opened existing,
             * 2 = created, 3 = replaced). For TYPE we always opened
             * an existing file; without this, MS-DOS 4 returns
             * "Invalid function" to the caller. */
            if (al == 0x2E) {
                r.w.cx = 1u;
            }

            r.w.flags &= ~1u;
            return;
        }

        case 0x08: { /* Read From File */
            uint8_t __far *sft;
            uint8_t __far *user_buf;
            uint16_t buf_seg, buf_off;
            uint32_t pos;
            int actual;
            struct open_slot *slot;

            g_ff_capture.read_call_count++;
            slot = find_open_slot(r.x.es, r.w.di);
            if (!slot) {
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }
            sft = (uint8_t __far *)MK_FP(r.x.es, r.w.di);
            /* User read buffer is at SDA+0x0C (DWORD), set by DOS in
             * DosRdWrSft just before calling us. NOT DS:DX. */
            buf_off = *(uint16_t __far *)(g_sda + SDA_DTA_OFF);
            buf_seg = *(uint16_t __far *)(g_sda + SDA_DTA_OFF + 2);
            user_buf = (uint8_t __far *)MK_FP(buf_seg, buf_off);
            pos = *(uint32_t __far *)(sft + SFT_FILE_POSITION_OFF);
            g_ff_capture.last_read_pos = pos;
            g_ff_capture.last_read_count = r.w.cx;

            actual = read_file_bytes(&slot->inode, pos, r.w.cx, user_buf);
            g_ff_capture.last_read_actual = (int16_t)actual;

            *(uint32_t __far *)(sft + SFT_FILE_POSITION_OFF) =
                pos + (uint32_t)actual;
            r.w.cx = (uint16_t)actual;
            r.w.flags &= ~1u;
            return;
        }

        case 0x0C: { /* Get Disk Space — real values from ext4 superblock */
            uint32_t blocks_total;
            uint32_t blocks_free;
            uint32_t bs;
            uint16_t spc;

            if (!g_fs_ready) {
                r.w.ax = 1u;   r.w.bx = 1u;
                r.w.cx = 512u; r.w.dx = 0u;
                r.w.flags &= ~1u;
                return;
            }

            bs = g_fs.sb.block_size;
            blocks_total = (uint32_t)g_fs.sb.blocks_count;
            blocks_free  = (uint32_t)g_fs.sb.free_blocks_count;
            spc = (uint16_t)(bs / 512u);
            if (spc == 0u) spc = 1u;
            /* DOS uses 16-bit cluster counts; cap. Real-world disks > 32 MB
             * with 1 KB blocks need cluster scaling; v1 just truncates. */
            if (blocks_total > 0xFFFFul) blocks_total = 0xFFFFul;
            if (blocks_free  > 0xFFFFul) blocks_free  = 0xFFFFul;

            /* Per FreeDOS dosfns.c (rg[0..3] = AX, BX, CX, DX):
             *   AX = sectors per cluster
             *   BX = total clusters
             *   CX = bytes per sector
             *   DX = free clusters
             * (Some RBIL sources order these differently — empirical.) */
            r.w.ax = spc;                    /* sectors/cluster */
            r.w.bx = (uint16_t)blocks_total; /* total clusters */
            r.w.cx = 512u;                   /* bytes/sector */
            r.w.dx = (uint16_t)blocks_free;  /* free clusters */
            r.w.flags &= ~1u;
            return;
        }

        case 0x23: /* Qualify Remote File Name (INT 2Fh AX=1123h)
                    *   DS:SI = ASCIIZ path to qualify
                    *   ES:DI = 67-byte output buffer for canonical name
                    *   Return CF=0 + canonical name in ES:DI if we own this
                    *   path, CF=1 to let DOS qualify locally.
                    *
                    * Two failure modes we hit getting this right:
                    *  - CF=0 unconditionally: FreeDOS shrugs, MS-DOS 4
                    *    takes the empty ES:DI buffer literally and the
                    *    subsequent OPEN of "garbage" fails — SYSINIT bails
                    *    with "Bad or missing Command Interpreter".
                    *  - CF=0 only for Y: paths but not writing ES:DI:
                    *    same garbage-buffer problem for Y: lookups
                    *    ("Invalid drive specification" from COMMAND.COM).
                    * Fix: only claim Y:-prefixed paths AND copy them into
                    * ES:DI verbatim (ours is already canonical). */
            {
                uint8_t __far *src = (uint8_t __far *)MK_FP(r.x.ds, r.w.si);
                uint8_t        c0  = src[0];
                /* Case-insensitive Y: match. */
                if (c0 >= 'a' && c0 <= 'z') c0 = (uint8_t)(c0 - 0x20);
                if (c0 == DRIVE_LETTER && src[1] == ':') {
                    uint8_t __far *dst = (uint8_t __far *)MK_FP(r.x.es, r.w.di);
                    int             i;
                    /* Force the drive letter to uppercase canonical 'Y:'. */
                    dst[0] = DRIVE_LETTER;
                    dst[1] = ':';
                    for (i = 2; i < 67; i++) {
                        dst[i] = src[i];
                        if (src[i] == 0) break;
                    }
                    r.w.flags &= ~1u;
                } else {
                    /* Not for us — leave qualification to DOS. */
                    r.w.flags |= 1u;
                }
            }
            return;

        case 0x18: /* FindFirst-alt (older DOS) */
        case 0x19: /* MS-DOS 4 IFS_SEQ_SEARCH_FIRST — sequential variant */
        case 0x1B:  /* FindFirst */
            if (!g_fs_ready) {
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }
            /* For volume-label searches, ext4 has no FAT-style volume entry —
             * the volume name lives in the superblock. Just say "no files". */
            if ((g_sda[SDA_SATTR_OFF] & 0x08u) != 0u) {
                r.w.ax = DOS_ERR_NO_MORE_FILES;
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

        case 0x1A: /* MS-DOS 4 IFS_SEQ_SEARCH_NEXT — sequential variant */
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

        case 0x2D: /* MS-DOS 4 Get/Set Extended Attributes (XA) — dispatched
                    * by INT 21h AH=57h AL=2..4 (FileTimes XA subfunctions)
                    * via HANDLE.ASM line 620: `MOV AX,(multNET SHL 8) or 45`.
                    *
                    * COMMAND.COM TYPE calls AX=5702h immediately after a
                    * successful Open to read the file's code-page tag. If
                    * we don't claim this call (CF=1), the kernel returns
                    * error_invalid_function to the caller and TYPE prints
                    * "Invalid function - <filename>". ext4 has no concept
                    * of DOS code-page extended attributes, so report none
                    * exist (CX=0 = empty XA list) and succeed. */
            r.w.cx = 0u;
            r.w.flags &= ~1u;
            return;

        default:
            /* Unknown AH=11h subfunction. DON'T claim it as ours — the
             * MS-DOS 4 kernel issues several extended subfunctions
             * (AL=0x22, 0x2B, 0x2D, 0x3E ...) that we don't implement,
             * and answering "file not found" for them confuses the
             * kernel's path-resolution / file-info plumbing on Y:
             * (visible symptom: COMMAND.COM TYPE Y:\file fails even
             * though DIR Y: works). Fall through to the chain so the
             * default DOS handler can deal with it. */
            break;
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

    if (!g_quiet) {
        printf("  LOL    : %04x:%04x\n", FP_SEG(lol), FP_OFF(lol));
        printf("  CDS arr: %04x:%04x\n", cds_seg, cds_off);
        printf("  LASTDRIVE byte at LOL+0x21 = %u\n", (unsigned)lastdrive);
    }

    if ((unsigned)lastdrive <= DRIVE_INDEX) {
        printf("  WARN: LASTDRIVE byte too low; trying %c: anyway\n", DRIVE_LETTER);
    }

    cds = cds_array + (unsigned)DRIVE_INDEX * CDS_ENTRY_SIZE;
    if (!g_quiet) {
        printf("  %c:CDS  : %04x:%04x  (slot %u)\n",
               DRIVE_LETTER, FP_SEG(cds), FP_OFF(cds), DRIVE_INDEX);
    }

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
    if (!g_quiet) {
        printf("  ext4 mounted: drive 0x%02x, partition LBA %lu\n",
               drive, (unsigned long)g_partition_lba);
        printf("  volume     : %s\n",
               g_fs.sb.volume_name[0] ? g_fs.sb.volume_name : "(unset)");
        printf("  blocks     : %lu (block size %lu)\n",
               (unsigned long)g_fs.sb.blocks_count,
               (unsigned long)g_fs.sb.block_size);
    }
    return 0;
}

int main(int argc, char **argv) {
    uint8_t drive = 0x81;
    char err[100];
    int ai;

    /* Parse args: [-q] [drive_num]. -q suppresses install banner (used
     * by CONFIG.SYS INSTALL= so the boot screen stays clean). Lenient
     * about leading dash/slash since MS-DOS 4 INSTALL= flattens args
     * differently from a normal exec. */
    for (ai = 1; ai < argc; ai++) {
        const char *a = argv[ai];
        while (*a == '-' || *a == '/') a++;
        if      (a[0] == 'q' || a[0] == 'Q') g_quiet = 1;
        else if (a[0] >= '0' && a[0] <= '9') drive = (uint8_t)strtoul(a, NULL, 0);
    }

    if (hook_cds() != 0) {
        return 1;
    }

    g_sda = get_sda();

    if (!g_quiet) {
        printf("ext4-dos TSR\n");
        printf("  drive %c: marked as redirector (flag 0x%04x)\n",
               DRIVE_LETTER, CDS_FLAG_REDIRECTED);
        printf("  SDA at %04x:%04x\n", FP_SEG(g_sda), FP_OFF(g_sda));
    }

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

    /* DO NOT hard-code a paragraph count here. Read the MCB and keep
     * ALL of our allocation.
     *
     * Why: in OpenWatcom small model the loaded image is code (~22 KB) +
     * static buffers (block cache, inode buf, extent node buf, dir block
     * buf, fs context) totaling well past 32 KB. Hard-coding _dos_keep
     * to anything less than our actual size silently truncates BSS, so
     * static structs at the high end of our segment (e.g. the singleton
     * blockdev / int13_ctx in src/blockdev/int13_bdev.c) land in the
     * area DOS reclaims. After install they get overwritten by whatever
     * DOS allocates next, which manifests as:
     *   - bd->sector_size == 0  → ext4_inode_read returns rc=-4
     *   - FindFirst returns "file not found" for every path
     *   - DIR Y: prints volume header and zero entries
     *   - TYPE Y:\HELLO.TXT silently produces no output
     * Mount itself succeeds at install time (banner shows real volume
     * + block count) because corruption only happens AFTER _dos_keep,
     * which makes this bug devious to diagnose.
     *
     * The MCB header sits one paragraph below our PSP; offset 3 holds
     * the allocated paragraph count DOS actually gave us at EXEC time.
     * Use that as the authoritative size — never a guess. */
    {
        uint8_t  __far *mcb       = (uint8_t  __far *)MK_FP(_psp - 1, 0);
        uint16_t __far *mcb_paras = (uint16_t __far *)(mcb + 3);
        _dos_keep(0, *mcb_paras);
    }
    return 0;
}
