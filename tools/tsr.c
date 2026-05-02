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

/* Drive letter is no longer a compile-time constant — TSR auto-picks the
 * first free CDS slot at install time (or honors a user override). Stored
 * here as a single byte ('A'..'Z') and one byte index (0..25). Both must
 * live in DGROUP because the resident handler reads them in interrupt
 * context where SS!=DS. */
static uint8_t g_drive_letter = 0;     /* 'A'..'Z'; 0 = uninitialised */
static uint8_t g_drive_index  = 0;     /* g_drive_letter - 'A' */

#define DOS_ERR_FILE_NOT_FOUND  0x02u
#define DOS_ERR_ACCESS_DENIED   0x05u
#define DOS_ERR_NO_MORE_FILES   0x12u
#define DOS_ERR_WRITE_PROTECT   0x13u

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
#define CDS_OFF_FLAGS       0x43   /* curdir_flags (word) */
#define CDS_OFF_BACKSLASH   0x4F   /* curdir_end (word) */

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

/* Snapshot of critical superblock fields, taken at install time.
 *
 * These are NON-ZERO-INITIALIZED on purpose — that places them in the
 * _DATA segment (low DGROUP offsets) instead of _BSS (high offsets,
 * 0x81C+). Some MS-DOS 4 code path writes to a fixed offset in our DS
 * around 0x8AA — likely a stale far pointer pointing at our segment with
 * a kernel-side scratch-buffer offset. The corruption silently clobbers
 * whatever sb field happens to live there (block_size, free_blocks_count,
 * etc.), making subsequent ext4 reads or GetDiskSpace replies break.
 *
 * Reading from these snapshots in the GetDiskSpace handler keeps that
 * reply stable even when the live sb has been corrupted.
 *
 * Zeroing curdir_ifs_hdr in hook_cds() killed the FIRST corruption path
 * (block_size's high word — see earlier commit). This is a residual path
 * we haven't fully traced. The DOSBox-X heavy-debugger trap shows MS-DOS 4
 * kernel code at CS=0F30 doing FAT time/date encoder writes through our
 * DS at offset 0x8AA/0x8AC. */
static uint32_t g_safe_block_size           = 1u;
static uint32_t g_safe_blocks_count_lo      = 1u;
static uint32_t g_safe_blocks_count_hi      = 1u;
static uint32_t g_safe_free_blocks_count_lo = 1u;
static uint32_t g_safe_free_blocks_count_hi = 1u;

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

/* Scratch buffer for REM_WRITE: copies the user's FAR buffer into
 * DGROUP so ext4_file_write_block (which expects a near pointer) can
 * read it. One block_size worth — phase 1b's first cut writes exactly
 * one block per call. */
static uint8_t g_write_buf[4096];

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
    /* Open/Read/Write/Close diagnostics */
    uint16_t open_call_count;
    uint16_t read_call_count;
    uint16_t write_call_count;
    uint16_t close_call_count;
    /* Last write attempt diagnostics — populated by REM_WRITE so
     * ext4dmp can show what happened (rc/err on refusal, success
     * params for the last accepted write). */
    int16_t  last_write_rc;
    char     last_write_err[64];
    uint8_t  last_write_was_extend; /* 0 = in-place, 1 = extend */
    uint32_t last_write_pos;
    uint16_t last_write_count;
    uint32_t last_write_file_size;
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
    /* Phase 3.5 diagnosis: tracks what DOS hands us on REM_CREATE vs the
     * subsequent REM_WRITE. If the SFT pointer (es:di) changes between
     * the two calls, find_open_slot misses and the new file stays
     * size 0 after a DOS COPY. The captured SFT bytes let us see
     * what DOS populated between our calls. */
    uint16_t last_create_ax;
    uint16_t last_create_seg;
    uint16_t last_create_off;
    uint32_t last_create_inode_num;
    uint8_t  last_create_sft_after[64];
    uint16_t last_write_seg;
    uint16_t last_write_off;
    uint8_t  last_write_slot_found;   /* 1 = find_open_slot hit, 0 = miss */
    uint8_t  last_write_sft_at_entry[64];
    /* Ring of last 8 REM_WRITE call snapshots (most recent in slot
     * write_log_idx-1 mod 8). Captures every entry, even count==0 and
     * slot-miss cases. */
    uint8_t  write_log_idx;
    struct {
        uint16_t es, di;
        uint16_t count;
        uint32_t pos_at_sft;
        uint32_t size_at_sft;
        uint32_t slot_inode_num;   /* 0 if slot lookup missed */
        uint8_t  slot_found;
    } write_log[8];
    /* Snapshot of register state + SDA at the LAST REM_WRITE entry,
     * so we can see whether the byte count lives in CX or in an SDA
     * field (DOS-C buffered-write convention). */
    uint16_t last_write_ax_in;
    uint16_t last_write_bx_in;
    uint16_t last_write_cx_in;
    uint16_t last_write_dx_in;
    uint16_t last_write_ds_in;
    uint16_t last_write_si_in;
    uint8_t  last_write_sda[128];
};
static struct ff_capture g_ff_capture;

static int is_leap_year(int year) {
    if (year % 400 == 0) return 1;
    if (year % 100 == 0) return 0;
    if (year %   4 == 0) return 1;
    return 0;
}

/* Map an ext4 inode's mode (file type + permission bits) and the FIRST
 * character of the basename to the DOS attribute byte used in FAT
 * directory entries and SFT.
 *
 * DOS attribute bits:
 *   0x01  R  read-only       — set when the owner-write bit is clear
 *   0x02  H  hidden          — set when the basename starts with '.'
 *                              (Unix dotfile convention; nothing in ext4
 *                              maps cleanly to DOS-style hidden, but this
 *                              gives users intuitive results)
 *   0x04  S  system          — never set (no ext4 equivalent)
 *   0x08  V  volume label    — never set (we have no FAT-style label)
 *   0x10  D  directory       — set when ext4 mode is S_IFDIR
 *   0x20  A  archive         — left clear; we're read-only, so "needs
 *                              backup since last archive" is meaningless
 *
 * Takes the first basename char by value (not a pointer): in INT 2Fh
 * handler context SS != DS, so passing &stack_local would have the
 * helper dereference through DS and read random kernel memory. Caller
 * passes 0 if no basename is available — the H bit just won't compute. */
static uint8_t ext4_mode_to_dos_attr(uint16_t mode, char basename_first_ch) {
    uint8_t attr = 0;
    if ((mode & EXT4_S_IFMT) == EXT4_S_IFDIR) attr |= 0x10u;   /* D */
    if ((mode & 0x0080u) == 0u)               attr |= 0x01u;   /* R */
    if (basename_first_ch == '.')             attr |= 0x02u;   /* H */
    return attr;
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
    if (src[0] != g_drive_letter || src[1] != ':') return -1;
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

/* Forward decl — to_8_3 below calls name_is_8_3_safe defined later. */
static int name_is_8_3_safe(const char *name, uint8_t name_len);

/* Cheap 16-bit case-insensitive hash for the ~XXX suffix on alias names. */
static uint16_t lfn_hash(const char *name, uint8_t name_len) {
    uint16_t h = 0;
    int i;
    for (i = 0; i < name_len; i++) {
        uint8_t c = (uint8_t)name[i];
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 'A');
        h = (uint16_t)((h * 17u) + c);
    }
    return h;
}

static int is_8_3_char(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (c == '_' || c == '-' || c == '~') return 1;
    return 0;
}

/* Convert an ext4 filename to the 11-byte FAT 8.3 form.
 *
 * Names that already fit 8.3 cleanly: trivial truncate + uppercase.
 *
 * Names too long or with 8.3-illegal chars (multiple dots, basename > 8,
 * extension > 3, foreign letters): generate a Win95-style alias
 * BASE4~HHH.EXT, where HHH is a 3-hex-digit hash of the FULL ext4 name.
 *
 * Deterministic — the same long name always maps to the same alias, so
 * `TYPE Y:\VERY~876.TXT` reopens the same file every time. Per-directory
 * collision rate is 1/4096 between any two long names that share the
 * first 4 8.3-safe characters; sufficient for typical DOS use. */
static void to_8_3(const char *name, uint8_t name_len, uint8_t out[11]) {
    int i = 0, j = 0, k;
    for (k = 0; k < 11; k++) out[k] = ' ';

    if (name_is_8_3_safe(name, name_len)) {
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
        return;
    }

    /* Hash-based alias path. */
    {
        static const char hex[] = "0123456789ABCDEF";
        uint16_t h;
        int last_dot = -1;
        int basename_end;

        for (i = 0; i < name_len; i++) {
            if (name[i] == '.') last_dot = i;
        }
        basename_end = (last_dot >= 0) ? last_dot : name_len;

        j = 0;
        for (i = 0; i < basename_end && j < 4; i++) {
            uint8_t c = (uint8_t)name[i];
            if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 'A');
            if (is_8_3_char(c)) out[j++] = c;
        }
        if (j == 0) out[j++] = '_';

        h = lfn_hash(name, name_len);
        out[4] = '~';
        out[5] = hex[(h >> 8) & 0xF];
        out[6] = hex[(h >> 4) & 0xF];
        out[7] = hex[(h >> 0) & 0xF];

        j = 8;
        if (last_dot >= 0) {
            for (i = last_dot + 1; i < name_len && j < 11; i++) {
                uint8_t c = (uint8_t)name[i];
                if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 'A');
                if (is_8_3_char(c)) out[j++] = c;
            }
        }
    }
}

struct find_iter_state {
    uint16_t target_index;
    uint16_t current_index;
    int      found;
    uint8_t  pattern[11];     /* 8.3-form search pattern from SDA's SDB */
    uint8_t  pattern_set;     /* 1 if caller populated `pattern`, 0 = match all */
    uint8_t  name_len;
    char     name[256];
    uint8_t  name83[11];      /* 8.3 form of the matched entry */
    uint32_t inode;
    uint8_t  file_type;
};

/* FAT 8.3 wildcard matching: each pattern position must equal the
 * corresponding name byte, except '?' matches any character (including
 * the trailing-space padding used for short names and missing
 * extensions). DOS pre-compiles user patterns like `*.TXT` into the
 * 11-byte form (`????????TXT`) and stuffs it into the SDB before
 * dispatching to us, so we don't need to expand `*` ourselves — just
 * compare. */
static int name83_matches_pattern(const uint8_t *name83, const uint8_t *pattern) {
    int i;
    for (i = 0; i < 11; i++) {
        if (pattern[i] == '?') continue;
        if (pattern[i] != name83[i]) return 0;
    }
    return 1;
}

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

/* Alias-match callback: iterates a directory and finds the entry whose
 * generated 8.3 alias matches a target 11-byte name. Used when exact
 * dir_lookup fails — lets `TYPE Y:\VERY~876.TXT` reopen the long-named
 * file `verylongname1.txt`. */
struct alias_lookup_state {
    const uint8_t *target_alias;   /* 11-byte 8.3 form (uppercase, padded) */
    uint32_t       found_inode;
};
static int alias_lookup_cb(const struct ext4_dir_entry *e, void *ud) {
    /* Static so &candidate resolves via DS, not SS (interrupt context). */
    static uint8_t candidate[11];
    struct alias_lookup_state *s = (struct alias_lookup_state *)ud;
    if (e->name_len == 1 && e->name[0] == '.') return 0;
    if (e->name_len == 2 && e->name[0] == '.' && e->name[1] == '.') return 0;
    to_8_3(e->name, e->name_len, candidate);
    if (memcmp(candidate, s->target_alias, 11) == 0) {
        s->found_inode = e->inode;
        return 1;
    }
    return 0;
}

/* Path-lookup wrapper that walks segment-by-segment, trying exact match
 * first and falling back to 8.3-alias match on each segment. Required for
 * `TYPE Y:\VERY~876.TXT` — the basename "very~876.txt" doesn't exist in
 * the directory, but it's the alias for "verylongname1.txt". */
static uint32_t path_lookup_with_alias(struct ext4_fs *fs, const char *path) {
    static struct ext4_inode dir_inode;
    static char    seg[256];
    static uint8_t target_alias[11];
    /* Static so &alias_state resolves via DS, not SS — alias_lookup_cb
     * dereferences `s` via near pointer, and in interrupt-handler context
     * SS != DS. Same reason ext4_dir_lookup keeps its lookup_state static. */
    static struct alias_lookup_state alias_state;
    uint32_t       cur_ino = 2u;
    uint32_t       next;
    const char    *p = path;
    const char    *seg_start;
    size_t         seg_len;

    while (*p == '/') p++;
    while (*p) {
        seg_start = p;
        while (*p && *p != '/') p++;
        seg_len = (size_t)(p - seg_start);
        if (seg_len == 0u) break;
        if (seg_len >= sizeof seg) return 0u;
        memcpy(seg, seg_start, seg_len);
        seg[seg_len] = '\0';

        if (ext4_inode_read(fs, cur_ino, &dir_inode) != 0) return 0u;
        if ((dir_inode.mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return 0u;

        next = ext4_dir_lookup(fs, &dir_inode, seg);
        if (next == 0u) {
            /* Exact match failed — try 8.3-alias match. */
            to_8_3(seg, (uint8_t)seg_len, target_alias);
            alias_state.target_alias = target_alias;
            alias_state.found_inode  = 0u;
            (void)ext4_dir_iter(fs, &dir_inode, alias_lookup_cb, &alias_state);
            next = alias_state.found_inode;
        }
        if (next == 0u) return 0u;
        cur_ino = next;

        while (*p == '/') p++;
    }
    return cur_ino;
}

static int find_iter_cb(const struct ext4_dir_entry *e, void *ud) {
    /* Static so &alias resolves via DS, not SS — interrupt-context SS!=DS
     * pitfall, same reason every other static buffer in this file is. */
    static uint8_t alias[11];
    struct find_iter_state *s = (struct find_iter_state *)ud;
    /* Skip . and .. — FAT root convention */
    if (e->name_len == 1 && e->name[0] == '.') return 0;
    if (e->name_len == 2 && e->name[0] == '.' && e->name[1] == '.') return 0;
    /* Names that don't fit 8.3 cleanly are NOT skipped — to_8_3() generates
     * a deterministic ~HHH alias so they're visible to classic 8.3 callers. */

    /* Filter by 8.3 search pattern (e.g. DIR Y:\*.TXT). pattern_set==0
     * means no filter — return everything. */
    to_8_3(e->name, e->name_len, alias);
    if (s->pattern_set && !name83_matches_pattern(alias, s->pattern)) {
        return 0;
    }

    if (s->current_index == s->target_index) {
        s->name_len  = e->name_len;
        memcpy(s->name, e->name, e->name_len);
        s->name[e->name_len] = '\0';
        memcpy(s->name83, alias, 11);
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
    /* Compile the user's 8.3 search pattern from the canonical path in
     * SDA+SDA_PRI_PATH_OFF (e.g. "Y:\*.TXT"). MS-DOS 4 doesn't fill the
     * SDB pattern field for redirector dispatch (FreeDOS sometimes does);
     * parsing the last path component and expanding `*` to `?` ourselves
     * works on both. Patterns of "*.*" or no extension match everything. */
    {
        uint8_t __far *src = (uint8_t __far *)(g_sda + SDA_PRI_PATH_OFF);
        int          basename_start = 0;
        int          src_len = 0;
        int          j;
        int          dot_at = -1;
        int          star_seen = 0;
        /* Walk the path, find last '\' or '/'. Skip the drive prefix
         * "X:" if present so a bare "Y:" or "Y:" + no path component
         * doesn't get treated as the basename. */
        while (src_len < 80 && src[src_len] != 0) src_len++;
        if (src_len >= 2 && src[1] == ':') basename_start = 2;
        for (j = 0; j < src_len; j++) {
            if (src[j] == '\\' || src[j] == '/') basename_start = j + 1;
        }
        /* Locate '.' within the basename (last one wins for things like
         * "verylongname1.txt" — but for 8.3 patterns there's only one). */
        for (j = basename_start; j < src_len; j++) {
            if (src[j] == '.') dot_at = j;
        }
        /* Initialise pattern to all '?' (= match anything). Then overlay
         * literal characters from the basename. */
        for (j = 0; j < 11; j++) state.pattern[j] = '?';
        state.pattern_set = 0u;

        /* Name field (positions 0..7). */
        {
            int p = 0;
            int end = (dot_at >= 0) ? dot_at : src_len;
            for (j = basename_start; j < end && p < 8; j++) {
                uint8_t c = src[j];
                if (c == '*') { star_seen = 1; break; }
                if (c == '?') { p++; continue; }
                if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 'A');
                state.pattern[p++] = c;
                state.pattern_set = 1u;
            }
            if (!star_seen) {
                /* Literal name shorter than 8: pad with space (so trailing
                 * '?'s in pattern would over-match). DOS convention: an
                 * exact basename must equal the entry's space-padded form. */
                for (; p < 8; p++) state.pattern[p] = ' ';
            }
        }
        /* Extension field (positions 8..10). */
        if (dot_at >= 0) {
            int p = 8;
            star_seen = 0;
            for (j = dot_at + 1; j < src_len && p < 11; j++) {
                uint8_t c = src[j];
                if (c == '*') { star_seen = 1; break; }
                if (c == '?') { p++; continue; }
                if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 'A');
                state.pattern[p++] = c;
                state.pattern_set = 1u;
            }
            if (!star_seen) {
                for (; p < 11; p++) state.pattern[p] = ' ';
            }
        }
        /* No basename at all (e.g. "Y:\") — match everything. */
        if (basename_start >= src_len) state.pattern_set = 0u;
    }
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

    /* Use the alias find_iter_cb already generated for the matched entry —
     * no need to recompute via to_8_3(). */
    for (i = 0; i < 11; i++) name83[i] = state.name83[i];

    sdb    = (uint8_t __far *)(g_sda + SDA_TMP_DM_OFF);
    dirent = (uint8_t __far *)(g_sda + SDA_SEARCH_DIR_OFF);

    /* Fill the 32-byte FAT-style dir entry. */
    for (i = 0; i < 32; i++) dirent[i] = 0;
    for (i = 0; i < 11; i++) dirent[DIR_NAME_OFF + i] = name83[i];
    /* Pass the first basename char by value — see ext4_mode_to_dos_attr
     * comment. state.name is in DGROUP (static via dir_iter), but the
     * helper signature avoids the pointer altogether. */
    dirent[DIR_ATTRIB_OFF] = ext4_mode_to_dos_attr(
        entry_inode.mode,
        (state.name_len > 0) ? (char)state.name[0] : 0);
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
    sdb[DM_DRIVE_OFF]      = (uint8_t)(g_drive_index | 0x80u);
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

    /* Canary probe — AX=11F0, BX=magic. Returns lo16 of LIVE g_fs.sb
     * fields in CX/DX/SI/DI; AL=0xFF on success. ext4chk /V compares
     * these against the install-time `_DATA` snapshots — if they
     * disagree, a stray kernel write has hit g_fs.sb. See the
     * "Defending against the residual kernel write" section in
     * docs/dos-internals.md. */
    if (r.w.ax == 0x11F0u && r.w.bx == EXT4_DOS_MAGIC_PROBE) {
        if (g_fs_ready) {
            r.w.cx = (uint16_t)(g_fs.sb.block_size & 0xFFFFul);
            r.w.dx = (uint16_t)((uint32_t)g_fs.sb.blocks_count & 0xFFFFul);
            r.w.si = (uint16_t)((uint32_t)g_fs.sb.free_blocks_count & 0xFFFFul);
            r.w.di = (uint16_t)(g_fs.sb.inodes_per_group & 0xFFFFul);
            r.h.al = 0xFFu;
        } else {
            r.h.al = 0u;
        }
        return;
    }

    /* Uninstall probe — AX=11FB, BX=magic. Restores the prev INT 2Fh
     * vector, clears the CDS flags so DOS stops dispatching to drive Y:,
     * and returns our resident PSP segment in SI so the caller can free
     * the memory via INT 21h AH=49h. The actual MCB free has to happen
     * from outside our handler (we'd be running in the freed memory
     * otherwise), so the user-side `ext4 -u` command does both halves:
     * it issues this probe and then frees PSP+env from its own context.
     * Returns AL=0xFF on success, AL=0 if a different handler is now
     * head of the INT 2Fh chain (someone hooked after us — refusing to
     * unhook prevents trampling another TSR's vector). */
    if (r.w.ax == 0x11FBu && r.w.bx == EXT4_DOS_MAGIC_PROBE) {
        void (__interrupt __far *current_2f)(void) = _dos_getvect(0x2F);
        if (FP_SEG(current_2f) != FP_SEG(my_int2f_handler) ||
            FP_OFF(current_2f) != FP_OFF(my_int2f_handler)) {
            r.h.al = 0;
            r.w.flags |= 1u;
            return;
        }
        _dos_setvect(0x2F, prev_int2f);
        if (g_cds_entry) {
            *(uint16_t __far *)(g_cds_entry + CDS_OFF_FLAGS) = 0;
        }
        r.w.si = _psp;
        r.h.al = 0xFFu;
        return;
    }

    /* Snapshot probe — AX=11EF, BX=magic. Returns lo16 of the
     * install-time `_DATA`-segment snapshots; AL=0xFF on success.
     * Used by ext4chk /V together with the canary above. */
    if (r.w.ax == 0x11EFu && r.w.bx == EXT4_DOS_MAGIC_PROBE) {
        r.w.cx = (uint16_t)(g_safe_block_size & 0xFFFFul);
        r.w.dx = (uint16_t)(g_safe_blocks_count_lo & 0xFFFFul);
        r.w.si = (uint16_t)(g_safe_free_blocks_count_lo & 0xFFFFul);
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
        case 0x05: /* ChDir — drive walk only, no disk write */
        case 0x07: /* Commit File — flush is a no-op for a read-only mount */
        case 0x0A: /* Lock Region — advisory; safe to silently succeed */
        case 0x0B: /* Unlock Region — same */
            r.w.flags &= ~1u;
            return;

        /* Read-only redirector — refuse every subfunction that would
         * mutate disk state. Returning error 0x13 (write protect) gives
         * DOS callers the canonical "this volume is read-only" experience:
         *   COPY foo Y:\bar     →  "Write protect error writing drive Y"
         *   DEL Y:\foo          →  "Write protect error"
         *   MD Y:\new           →  "Unable to create directory"
         * Without these arms the calls fall through to `_chain_intr`,
         * where the DOS default handler returns inscrutable errors.
         *
         * Subfunction numbers per references/freedos-kernel/hdr/network.h
         * (REM_RMDIR=0x01, REM_MKDIR=0x03, REM_WRITE=0x09, REM_SETATTR=0x0E,
         * REM_RENAME=0x11, REM_DELETE=0x13, REM_CREATE=0x17,
         * REM_CRTRWOCDS=0x18). 0x21 LSEEK is handled separately in the
         * stub below — DOS uses it for SeekEnd-to-find-EOF, which is fine
         * to support read-only via the SFT's file-size field. */
        case 0x01: { /* REM_RMDIR — remove directory (Phase 4.5). */
            static char path_buf[128];
            static char werr[64];
            static char parent_path[128];
            uint32_t dir_ino, parent_ino;
            int rc_path, p, base_idx, j;

            if (!g_fs_ready) {
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }
            rc_path = dos_to_ext4_path(path_buf, sizeof path_buf);
            if (rc_path != 0) {
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }
            dir_ino = path_lookup_with_alias(&g_fs, path_buf);
            if (dir_ino == 0) {
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }
            base_idx = 0;
            for (p = 0; path_buf[p]; p++)
                if (path_buf[p] == '/') base_idx = p + 1;
            if (base_idx <= 1) {
                parent_ino = 2u;
            } else {
                for (j = 0; j + 1 < base_idx && j < 127; j++)
                    parent_path[j] = path_buf[j];
                parent_path[j] = '\0';
                parent_ino = ext4_path_lookup(&g_fs, parent_path);
                if (parent_ino == 0) {
                    r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                    r.w.flags |= 1u;
                    return;
                }
            }
            werr[0] = '\0';
            if (ext4_dir_remove(&g_fs, parent_ino, dir_ino, werr, sizeof werr) != 0) {
                r.w.ax = DOS_ERR_WRITE_PROTECT;
                r.w.flags |= 1u;
                return;
            }
            r.w.flags &= ~1u;
            return;
        }

        case 0x0E: /* REM_SETATTR — change file attributes */
        case 0x11: /* REM_RENAME */
        case 0x13: /* REM_DELETE */
        /* 0x17 = REM_CREATE: now handled above — do not fall through here */
            /* 0x18 deliberately NOT here: FreeDOS's network.h calls it
             * REM_CRTRWOCDS (create-R/W-without-CDS), but other references
             * map it to a FindFirst-alt that older DOS issues. The
             * existing FindFirst-alt arm below handles the latter; if
             * FreeDOS ever dispatches the create variant here we'd want
             * to refuse it, but the smoke test doesn't exercise the path. */
            r.w.ax = DOS_ERR_WRITE_PROTECT;
            r.w.flags |= 1u;
            return;

        case 0x03: { /* REM_MKDIR — create a new directory (Phase 4). */
            static char path_buf[128];
            static char werr[64];
            uint32_t parent_ino;
            uint32_t new_ino;
            int      rc_path, p, base_idx;
            uint8_t  name_len;
            uint32_t now_unix;

            if (!g_fs_ready) {
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }
            rc_path = dos_to_ext4_path(path_buf, sizeof path_buf);
            if (rc_path != 0) {
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }
            /* Refuse if it already exists. */
            if (path_lookup_with_alias(&g_fs, path_buf) != 0) {
                r.w.ax = 5u; /* ACCESS_DENIED */
                r.w.flags |= 1u;
                return;
            }
            /* Split path into parent + basename. */
            base_idx = 0;
            for (p = 0; path_buf[p]; p++)
                if (path_buf[p] == '/') base_idx = p + 1;
            if (base_idx <= 1) {
                parent_ino = 2u;
            } else {
                static char parent_path[128];
                int j;
                for (j = 0; j + 1 < base_idx && j < 127; j++)
                    parent_path[j] = path_buf[j];
                parent_path[j] = '\0';
                parent_ino = ext4_path_lookup(&g_fs, parent_path);
                if (parent_ino == 0) {
                    r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                    r.w.flags |= 1u;
                    return;
                }
            }
            name_len = 0;
            while (path_buf[base_idx + name_len]) name_len++;
            if (name_len == 0) {
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }
            now_unix = 0x6A000000u;
            werr[0] = '\0';
            new_ino = ext4_dir_create(&g_fs, parent_ino,
                                      path_buf + base_idx, name_len,
                                      now_unix, werr, sizeof werr);
            if (new_ino == 0) {
                r.w.ax = DOS_ERR_WRITE_PROTECT;
                r.w.flags |= 1u;
                return;
            }
            r.w.flags &= ~1u;
            return;
        }

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

        case 0x17: { /* REM_CREATE — create a new file (or open existing for r/w).
                      * Phase 3 first cut: create new file in a linear (non-htree)
                      * parent directory. If file already exists: refuse with access
                      * denied (no truncate yet — phase 3.5). */
            static char path_buf[128];
            static char werr[64];
            uint8_t __far *sft;
            uint32_t       new_inode_num;
            uint32_t       parent_ino;
            int            rc_path;
            int            base_idx;
            int            p;
            uint8_t        name_len;
            uint32_t       now_unix;
            struct open_slot *slot;

            if (!g_fs_ready) {
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }
            rc_path = dos_to_ext4_path(path_buf, sizeof path_buf);
            if (rc_path != 0) {
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }

            /* Split path: find last '/' to get parent + basename. */
            base_idx = 0;
            for (p = 0; path_buf[p]; p++) {
                if (path_buf[p] == '/') base_idx = p + 1;
            }
            /* Refuse if file already exists. */
            if (path_lookup_with_alias(&g_fs, path_buf) != 0) {
                r.w.ax = 5u; /* ACCESS_DENIED */
                r.w.flags |= 1u;
                return;
            }
            /* Resolve parent directory. base_idx is one past the last '/'.
             * Copy path_buf[0..base_idx-2] (strips trailing slash). */
            if (base_idx <= 1) {
                parent_ino = 2u; /* root — file is directly in '/' */
            } else {
                static char parent_path[128];
                int j;
                /* Copy up to but NOT including the trailing '/' at base_idx-1. */
                for (j = 0; j + 1 < base_idx && j < 127; j++)
                    parent_path[j] = path_buf[j];
                parent_path[j] = '\0';
                parent_ino = ext4_path_lookup(&g_fs, parent_path);
                if (parent_ino == 0) {
                    r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                    r.w.flags |= 1u;
                    return;
                }
            }
            /* name_len and pointer. */
            name_len = 0;
            while (path_buf[base_idx + name_len]) name_len++;
            if (name_len == 0) {
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }

            /* TSR has no wall clock; use a fixed timestamp (~2026). */
            now_unix = 0x6A000000u;

            slot = alloc_open_slot(r.x.es, r.w.di);
            if (!slot) {
                r.w.ax = 4u; /* TOO_MANY_OPEN_FILES */
                r.w.flags |= 1u;
                return;
            }

            werr[0] = '\0';
            new_inode_num = ext4_file_create(&g_fs, parent_ino,
                                             path_buf + base_idx, name_len,
                                             (uint16_t)(0x8000u | 0644u), now_unix,
                                             werr, sizeof werr);
            if (new_inode_num == 0) {
                slot->used = 0;
                r.w.ax = DOS_ERR_WRITE_PROTECT;
                r.w.flags |= 1u;
                return;
            }

            /* Initialise slot with the new (empty) inode. */
            slot->used      = 1;
            slot->sft_seg   = r.x.es;
            slot->sft_off   = r.w.di;
            slot->inode_num = new_inode_num;
            /* Build a minimal in-memory inode for the slot (no disk re-read). */
            memset(&slot->inode, 0, sizeof slot->inode);
            slot->inode.mode  = (uint16_t)(0x8000u | 0644u);
            slot->inode.size  = 0u;
            slot->inode.mtime = now_unix;
            slot->inode.flags = EXT4_INODE_FLAG_EXTENTS;
            /* i_block: extent header (entries=0) */
            slot->inode.i_block[0] = (uint8_t)EXT4_EXT_MAGIC;
            slot->inode.i_block[1] = (uint8_t)(EXT4_EXT_MAGIC >> 8);
            slot->inode.i_block[4] = 4u; /* max = 4 */

            sft = (uint8_t __far *)MK_FP(r.x.es, r.w.di);
            *(uint16_t __far *)(sft + SFT_REF_COUNT_OFF) = 1u;
            *(uint16_t __far *)(sft + SFT_DEVINFO_OFF)   =
                (uint16_t)(0x8000u | g_drive_index);
            sft[SFT_FILE_ATTR_OFF] = 0x00u; /* normal file */
            {
                uint32_t td = unix_to_dos(now_unix);
                *(uint16_t __far *)(sft + SFT_FILE_TIME_OFF) =
                    (uint16_t)(td & 0xFFFFul);
                *(uint16_t __far *)(sft + SFT_FILE_DATE_OFF) =
                    (uint16_t)(td >> 16);
            }
            *(uint32_t __far *)(sft + SFT_FILE_SIZE_OFF)     = 0u;
            *(uint32_t __far *)(sft + SFT_FILE_POSITION_OFF) = 0u;

            /* Phase 3.5 diagnosis: snapshot what DOS handed us + what we
             * wrote back, so a later REM_WRITE that misses the slot can
             * be compared. */
            g_ff_capture.last_create_ax        = r.w.ax;
            g_ff_capture.last_create_seg       = r.x.es;
            g_ff_capture.last_create_off       = r.w.di;
            g_ff_capture.last_create_inode_num = new_inode_num;
            {
                uint16_t k;
                for (k = 0; k < 64; k++)
                    g_ff_capture.last_create_sft_after[k] = sft[k];
            }

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
            inode_num = path_lookup_with_alias(&g_fs, path_buf);
            g_ff_capture.last_open_inode_num = inode_num;
            if (inode_num == 0) {
                /* AL=0x2E is MS-DOS 4's "Extended Open" dispatch from
                 * AH=6C00h.  When the file doesn't exist MS-DOS 4 routes
                 * here instead of AL=0x17 (REM_CREATE) — the distinction
                 * in the DOS-4 source is EXTOPEN_ON being set in
                 * CREATE.ASM:IFS_extopen.  Fall through to create the
                 * file so that COPY to Y: works the same as on FreeDOS. */
                if (al == 0x2E) {
                    static char werr_eo[64];
                    static char parent_eo[128];
                    uint32_t parent_ino;
                    uint32_t new_ino;
                    int p, base_idx, j;
                    uint8_t nm_len;
                    uint8_t __far *sft_eo;
                    struct open_slot *slot_eo;
                    uint32_t now_eo = 0x6A000000u;

                    base_idx = 0;
                    for (p = 0; path_buf[p]; p++)
                        if (path_buf[p] == '/') base_idx = p + 1;
                    if (base_idx <= 1) {
                        parent_ino = 2u;
                    } else {
                        for (j = 0; j + 1 < base_idx && j < 127; j++)
                            parent_eo[j] = path_buf[j];
                        parent_eo[j] = '\0';
                        parent_ino = ext4_path_lookup(&g_fs, parent_eo);
                        if (parent_ino == 0) goto extopen_notfound;
                    }
                    nm_len = 0;
                    while (path_buf[base_idx + nm_len]) nm_len++;
                    if (nm_len == 0) goto extopen_notfound;

                    slot_eo = alloc_open_slot(r.x.es, r.w.di);
                    if (!slot_eo) {
                        r.w.ax = 4u;
                        r.w.flags |= 1u;
                        return;
                    }
                    werr_eo[0] = '\0';
                    new_ino = ext4_file_create(&g_fs, parent_ino,
                                               path_buf + base_idx, nm_len,
                                               (uint16_t)(0x8000u | 0644u),
                                               now_eo, werr_eo, sizeof werr_eo);
                    if (new_ino == 0) {
                        slot_eo->used = 0;
                        goto extopen_notfound;
                    }
                    slot_eo->used      = 1;
                    slot_eo->sft_seg   = r.x.es;
                    slot_eo->sft_off   = r.w.di;
                    slot_eo->inode_num = new_ino;
                    memset(&slot_eo->inode, 0, sizeof slot_eo->inode);
                    slot_eo->inode.mode  = (uint16_t)(0x8000u | 0644u);
                    slot_eo->inode.mtime = now_eo;
                    slot_eo->inode.flags = EXT4_INODE_FLAG_EXTENTS;
                    slot_eo->inode.i_block[0] = (uint8_t)EXT4_EXT_MAGIC;
                    slot_eo->inode.i_block[1] = (uint8_t)(EXT4_EXT_MAGIC >> 8);
                    slot_eo->inode.i_block[4] = 4u;

                    sft_eo = (uint8_t __far *)MK_FP(r.x.es, r.w.di);
                    *(uint16_t __far *)(sft_eo + SFT_REF_COUNT_OFF) = 1u;
                    *(uint16_t __far *)(sft_eo + SFT_DEVINFO_OFF)   =
                        (uint16_t)(0x8000u | g_drive_index);
                    sft_eo[SFT_FILE_ATTR_OFF] = 0x00u;
                    {
                        uint32_t td = unix_to_dos(now_eo);
                        *(uint16_t __far *)(sft_eo + SFT_FILE_TIME_OFF) =
                            (uint16_t)(td & 0xFFFFul);
                        *(uint16_t __far *)(sft_eo + SFT_FILE_DATE_OFF) =
                            (uint16_t)(td >> 16);
                    }
                    *(uint32_t __far *)(sft_eo + SFT_FILE_SIZE_OFF)     = 0u;
                    *(uint32_t __far *)(sft_eo + SFT_FILE_POSITION_OFF) = 0u;
                    r.w.cx = 2u; /* action taken: file created */
                    r.w.flags &= ~1u;
                    return;
                }
extopen_notfound:
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
                (uint16_t)(0x8000u | g_drive_index);
            /* path_buf is the static dos_to_ext4_path output. Find the
             * char right after the last '/' — that's the basename's
             * first character, which the helper takes by value. */
            {
                int p = 0, base_idx = 0;
                while (path_buf[p]) {
                    if (path_buf[p] == '/') base_idx = p + 1;
                    p++;
                }
                sft[SFT_FILE_ATTR_OFF] = ext4_mode_to_dos_attr(
                    slot->inode.mode,
                    path_buf[base_idx]);
            }
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

        case 0x09: { /* REM_WRITE — phase 1b: in-place same-length only.
                      *
                      * Phase 1b refuses anything that would extend the
                      * file or hit a write that's not exactly one
                      * aligned FS block. With those constraints the
                      * write is a single 2-block transaction (file
                      * data + inode-with-mtime-bumped) through
                      * ext4_journal_commit, immediately checkpointed
                      * to disk. Phases 2+ relax the constraints (block
                      * allocation for extends, partial-block writes,
                      * etc). */
            struct open_slot *slot;
            uint8_t __far    *sft;
            uint8_t __far    *user_buf;
            uint16_t          buf_seg, buf_off;
            uint32_t          pos, file_size;
            uint16_t          count, i;
            uint32_t          bs;
            uint32_t          logical;
            uint32_t          now_unix;
            /* Static so it lives in DGROUP — the function takes a near
             * char*, but stack lives at SS != DS in interrupt context. */
            static char       werr[64];
            int               rc;

            g_ff_capture.write_call_count++;
            /* Phase 3.5 diagnosis: snapshot the entry SFT pointer + 64
             * bytes of SFT + register state + 128 bytes of SDA, every
             * call, to see how DOS conveys the byte count and user
             * buffer pointer. */
            g_ff_capture.last_write_seg = r.x.es;
            g_ff_capture.last_write_off = r.w.di;
            g_ff_capture.last_write_ax_in = r.w.ax;
            g_ff_capture.last_write_bx_in = r.w.bx;
            g_ff_capture.last_write_cx_in = r.w.cx;
            g_ff_capture.last_write_dx_in = r.w.dx;
            g_ff_capture.last_write_ds_in = r.x.ds;
            g_ff_capture.last_write_si_in = r.w.si;
            {
                uint8_t __far *sft_dbg = (uint8_t __far *)MK_FP(r.x.es, r.w.di);
                uint16_t       k;
                for (k = 0; k < 64; k++)
                    g_ff_capture.last_write_sft_at_entry[k] = sft_dbg[k];
                for (k = 0; k < 128; k++)
                    g_ff_capture.last_write_sda[k] = g_sda[k];
            }
            slot = find_open_slot(r.x.es, r.w.di);
            /* Ring-log this call regardless of whether slot lookup hit. */
            {
                uint8_t        idx     = g_ff_capture.write_log_idx & 7u;
                uint8_t __far *sft_log = (uint8_t __far *)MK_FP(r.x.es, r.w.di);
                g_ff_capture.write_log[idx].es        = r.x.es;
                g_ff_capture.write_log[idx].di        = r.w.di;
                g_ff_capture.write_log[idx].count     = r.w.cx;
                g_ff_capture.write_log[idx].pos_at_sft  =
                    *(uint32_t __far *)(sft_log + SFT_FILE_POSITION_OFF);
                g_ff_capture.write_log[idx].size_at_sft =
                    *(uint32_t __far *)(sft_log + SFT_FILE_SIZE_OFF);
                g_ff_capture.write_log[idx].slot_inode_num =
                    slot ? slot->inode_num : 0u;
                g_ff_capture.write_log[idx].slot_found = slot ? 1u : 0u;
                g_ff_capture.write_log_idx = (uint8_t)((idx + 1u) & 7u);
            }
            if (!slot) {
                g_ff_capture.last_write_slot_found = 0u;
                r.w.ax = DOS_ERR_FILE_NOT_FOUND;
                r.w.flags |= 1u;
                return;
            }
            g_ff_capture.last_write_slot_found = 1u;
            sft       = (uint8_t __far *)MK_FP(r.x.es, r.w.di);
            bs        = g_safe_block_size;
            pos       = *(uint32_t __far *)(sft + SFT_FILE_POSITION_OFF);
            file_size = (uint32_t)slot->inode.size;
            count     = r.w.cx;

            if (bs > sizeof g_write_buf) {
                r.w.ax = DOS_ERR_WRITE_PROTECT;
                r.w.flags |= 1u;
                return;
            }
            /* CX=0 in REM_WRITE means "set EOF to current SFT position" —
             * the documented MS-DOS network redirector convention. DOS COPY
             * uses this as a pre-extend before the data writes. Loop calling
             * extend_block one block at a time until size reaches pos; each
             * iteration requires the current size to be block-aligned.
             * Truncate-down (pos < file_size) is still deferred. */
            if (count == 0u) {
                if (pos == file_size) {
                    r.w.ax = 0u;
                    r.w.flags &= ~1u;
                    return;
                }
                if (pos > file_size && (file_size & (bs - 1u)) == 0u) {
                    static char werr2[64];
                    int rc2 = 0;
                    uint16_t z;
                    for (z = 0; z < bs; z++) g_write_buf[z] = 0u;
                    while ((uint32_t)slot->inode.size < pos && rc2 == 0) {
                        uint32_t remaining = pos - (uint32_t)slot->inode.size;
                        uint32_t this_ext  = (remaining > bs) ? bs : remaining;
                        werr2[0] = '\0';
                        rc2 = ext4_file_extend_block(&g_fs, &slot->inode,
                                                     slot->inode_num,
                                                     g_write_buf, this_ext,
                                                     slot->inode.mtime + 1u,
                                                     werr2, sizeof werr2);
                    }
                    g_ff_capture.last_write_rc = (int16_t)rc2;
                    if (rc2 == 0 && (uint32_t)slot->inode.size == pos) {
                        *(uint32_t __far *)(sft + SFT_FILE_SIZE_OFF) =
                            (uint32_t)slot->inode.size;
                        r.w.ax = 0u;
                        r.w.flags &= ~1u;
                        return;
                    }
                }
                /* Truncate-down or extend failed: defer. */
                r.w.ax = DOS_ERR_WRITE_PROTECT;
                r.w.flags |= 1u;
                return;
            }

            /* User buffer FAR pointer at SDA+0x0C, same convention DOS
             * uses for REM_READ (set in DosRdWrSft). */
            buf_off  = *(uint16_t __far *)(g_sda + SDA_DTA_OFF);
            buf_seg  = *(uint16_t __far *)(g_sda + SDA_DTA_OFF + 2);
            user_buf = (uint8_t __far *)MK_FP(buf_seg, buf_off);

            /* FAR -> DGROUP copy. ext4_file_write_block / extend_block
             * expect a near pointer (DS-relative) so the bdev_write
             * FAR conversion inside int13_bdev_write produces the
             * right segment. */
            for (i = 0; i < count; i++) g_write_buf[i] = user_buf[i];

            logical  = pos / bs;
            /* No safe wall-clock from inside the redirector; bump mtime
             * monotonically. Phase 1c can swap in a real time source. */
            now_unix = slot->inode.mtime + 1u;

            g_ff_capture.last_write_pos       = pos;
            g_ff_capture.last_write_count     = count;
            g_ff_capture.last_write_file_size = file_size;
            werr[0] = '\0';

            if (pos + (uint32_t)count <= file_size
                && (pos & (bs - 1u)) == 0u
                && ((uint32_t)count & (bs - 1u)) == 0u) {
                /* Block-aligned in-place write — one or more full blocks.
                 * Loop one block at a time so g_write_buf (1024 bytes max)
                 * is reused per block. Handles COPY with large transfer
                 * buffers that send N*bs bytes in a single REM_WRITE. */
                uint32_t blk_off = 0u;
                g_ff_capture.last_write_was_extend = 0u;
                rc = 0;
                while (blk_off < (uint32_t)count && rc == 0) {
                    uint16_t k;
                    for (k = 0; k < (uint16_t)bs; k++)
                        g_write_buf[k] = user_buf[(uint16_t)blk_off + k];
                    rc = ext4_file_write_block(&g_fs, &slot->inode,
                                               slot->inode_num,
                                               logical + blk_off / bs,
                                               g_write_buf, now_unix,
                                               werr, sizeof werr);
                    blk_off += bs;
                }
            } else if (pos + (uint32_t)count <= file_size
                       && ((pos / bs) == ((pos + (uint32_t)count - 1u) / bs))) {
                /* Partial in-place write contained within one block —
                 * read existing block into g_write_buf (overwriting the
                 * user-data we copied at offset 0), then overlay user
                 * data at the in-block offset. Used by DOS COPY's
                 * post-pre-extend data writes (Phase 3.5). */
                uint16_t off_in_block = (uint16_t)(pos & (bs - 1u));
                g_ff_capture.last_write_was_extend = 0u;
                rc = ext4_file_read_block(&g_fs, &slot->inode, logical,
                                          g_write_buf);
                if (rc == 0) {
                    for (i = 0; i < count; i++)
                        g_write_buf[off_in_block + i] = user_buf[i];
                    rc = ext4_file_write_block(&g_fs, &slot->inode,
                                               slot->inode_num, logical,
                                               g_write_buf, now_unix,
                                               werr, sizeof werr);
                }
            } else if (pos == file_size && (pos & (bs - 1u)) == 0u) {
                /* Append at EOF — may be partial (count < bs).
                 * Zero-pad g_write_buf beyond count bytes; extend_block
                 * writes the full block but inode.size advances by count. */
                {
                    uint16_t z;
                    for (z = count; z < bs; z++) g_write_buf[z] = 0u;
                }
                g_ff_capture.last_write_was_extend = 1u;
                rc = ext4_file_extend_block(&g_fs, &slot->inode, slot->inode_num,
                                            g_write_buf, (uint32_t)count, now_unix,
                                            werr, sizeof werr);
            } else {
                /* Sparse-hole or non-contiguous extend — phase 2.5+. */
                g_ff_capture.last_write_rc = -42;
                memcpy(g_ff_capture.last_write_err, "sparse-hole or non-contiguous extend", 36);
                g_ff_capture.last_write_err[36] = '\0';
                r.w.ax = DOS_ERR_WRITE_PROTECT;
                r.w.flags |= 1u;
                return;
            }
            g_ff_capture.last_write_rc = (int16_t)rc;
            /* Copy 64 bytes unconditionally — if werr has content the
             * dump's printable-only print will show it; if it doesn't,
             * we'll see all-zero in the hex dump. */
            memcpy(g_ff_capture.last_write_err, werr, sizeof g_ff_capture.last_write_err);
            if (rc != 0) {
                r.w.ax = DOS_ERR_WRITE_PROTECT;
                r.w.flags |= 1u;
                return;
            }

            *(uint32_t __far *)(sft + SFT_FILE_POSITION_OFF) = pos + count;
            /* Reflect any size change back into the SFT so DOS shows the
             * new size on close (extend_block updated slot->inode.size). */
            *(uint32_t __far *)(sft + SFT_FILE_SIZE_OFF) =
                (uint32_t)slot->inode.size;
            r.w.cx = count;
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

        case 0x0C: { /* Get Disk Space — real values from ext4 superblock.
                      *
                      * DOS reports free space as four 16-bit numbers:
                      *   AX = sectors per cluster
                      *   BX = total clusters
                      *   CX = bytes per sector
                      *   DX = free clusters
                      * (Per FreeDOS dosfns.c rg[0..3] — some RBIL sources
                      * disagree on order; this layout is empirical.)
                      *
                      * The cluster counts are 16-bit. For ext4 disks with
                      * more than 65535 blocks we'd lose precision if we
                      * truncated. Instead we DOUBLE the sectors-per-cluster
                      * (and halve the cluster counts) until both counts
                      * fit. The total bytes (spc × clusters × bps) stays
                      * the same — we're just expressing the same total in
                      * a different unit.
                      *
                      * Future: handle AX=11A3h "Get Extended Free Space"
                      * for callers that want full 32-bit precision (Win95+
                      * AX=7303h dispatches there). */
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

            /* Read from the install-time snapshot rather than g_fs.sb —
             * see comment near g_safe_block_size definition. */
            bs           = g_safe_block_size;
            blocks_total = g_safe_blocks_count_lo;
            blocks_free  = g_safe_free_blocks_count_lo;
            spc = (uint16_t)(bs / 512u);
            if (spc == 0u) spc = 1u;

            /* Scale spc up if either count overflows 16 bits, then clamp.
             * Cap spc at 64 — beyond that DOS apps may stumble on >32 KB
             * clusters. Total rounds up (avoid claiming non-existent
             * space), free rounds down (conservative).
             *
             * Earlier this was gated behind a `blocks_total > 0x18000`
             * threshold to dodge an MS-DOS 4 "bytes free" misdisplay seen
             * on small disks. The real cause was kernel-side corruption of
             * sb fields under MS-DOS 4 (see g_safe_block_size); now we
             * read from the safe snapshot, the empirical gate is no
             * longer needed. */
            while ((blocks_total > 0xFFFFul || blocks_free > 0xFFFFul)
                   && spc < 64u) {
                spc *= 2u;
                blocks_total = (blocks_total + 1u) >> 1;
                blocks_free  = blocks_free  >> 1;
            }
            if (blocks_total > 0xFFFFul) blocks_total = 0xFFFFul;
            if (blocks_free  > 0xFFFFul) blocks_free  = 0xFFFFul;

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
                if (c0 == g_drive_letter && src[1] == ':') {
                    uint8_t __far *dst = (uint8_t __far *)MK_FP(r.x.es, r.w.di);
                    int             i;
                    /* Force the drive letter to uppercase canonical form. */
                    dst[0] = g_drive_letter;
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

    if ((unsigned)lastdrive <= g_drive_index) {
        printf("  WARN: LASTDRIVE byte too low; trying %c: anyway\n", g_drive_letter);
    }

    cds = cds_array + (unsigned)g_drive_index * CDS_ENTRY_SIZE;
    if (!g_quiet) {
        printf("  %c:CDS  : %04x:%04x  (slot %u)\n",
               g_drive_letter, FP_SEG(cds), FP_OFF(cds), g_drive_index);
    }

    /* Zero the WHOLE 88-byte CDS slot — not just the 67-byte path field.
     *
     * MS-DOS 4's CDS extends past the path/flags/backslash fields with an
     * "IFS" block:
     *   off 0x51  curdir_type     byte (0=local FAT, 2=IFS, 4=netuse)
     *   off 0x52  curdir_ifs_hdr  4-byte far ptr to a File System Header
     *   off 0x56  curdir_fsda     2-byte File System Dependent area
     *
     * MS-DOS 4 uses the SAME bit value (0x8000) for `curdir_isnet` AND
     * `curdir_isifs`. So when we set the redirector flag, the kernel's IFS
     * dispatcher follows `curdir_ifs_hdr` as a far pointer to load segment
     * registers — and if those bytes were left uninitialised, the kernel
     * dereferences garbage and ends up with DS pointing at our DGROUP.
     * Subsequent writes by kernel code (e.g. the FAT time-encoder around
     * 0F30:0256 packing the directory entry's mtime) land inside our static
     * data, corrupting whatever happens to live at the targeted DGROUP
     * offset. The crash was layout-sensitive only because shifting our
     * statics changed which field got hit (commonly g_fs.sb.block_size's
     * upper word, which broke every subsequent ext4 sector calculation).
     *
     * Found via DOSBox-X heavy debugger `BPM` on &g_fs.sb.block_size. The
     * trap fired in MS-DOS 4 kernel code (CS=0F30) with our DGROUP in DS.
     * FreeDOS doesn't use curdir_ifs_hdr at all, which is why the bug only
     * showed up under MS-DOS 4. See references/msdos4/v4.0/src/INC/CURDIR.INC.
     *
     * Just zeroing curdir_ifs_hdr (and the rest) is enough — leaving the
     * IFS pointer at 0:0 makes the kernel skip the IFS dispatch path. */
    for (i = 0; i < CDS_ENTRY_SIZE; i++) cds[i] = 0;

    cds[CDS_OFF_PATH + 0] = g_drive_letter;
    cds[CDS_OFF_PATH + 1] = ':';
    cds[CDS_OFF_PATH + 2] = '\\';
    cds[CDS_OFF_PATH + 3] = 0;

    *(uint16_t __far *)(cds + CDS_OFF_BACKSLASH) = 2u;
    *(uint16_t __far *)(cds + CDS_OFF_FLAGS) = CDS_FLAG_REDIRECTED;

    g_cds_entry = cds;
    return 0;
}

/* Walk the CDS array and pick the first slot whose flags == 0 (no
 * driver currently owns it — neither physical FAT nor another
 * redirector). Start at D: so we don't disturb floppies/system drive.
 * Returns 'A'..'Z' on success, 0 if no free slot was found below
 * LASTDRIVE. */
static uint8_t pick_drive_letter(void) {
    char __far *lol = get_lol();
    uint16_t cds_off = *(uint16_t __far *)(lol + 0x16);
    uint16_t cds_seg = *(uint16_t __far *)(lol + 0x18);
    char __far *cds_array = (char __far *)MK_FP(cds_seg, cds_off);
    uint8_t lastdrive = *(uint8_t __far *)(lol + 0x21);
    uint8_t i;

    /* lastdrive is the count, not the highest index — slots are
     * 0..lastdrive-1. Start at D: (index 3): A:/B: are floppies, C: is
     * the boot disk, all of which DOS pre-flags. */
    for (i = 3; i < lastdrive; i++) {
        char __far *slot = cds_array + (unsigned)i * CDS_ENTRY_SIZE;
        uint16_t    flags = *(uint16_t __far *)(slot + CDS_OFF_FLAGS);
        if (flags == 0) {
            return (uint8_t)('A' + i);
        }
    }
    return 0;
}

static int mount_ext4(uint8_t drive, int quiet_fail);

/* Probe BIOS hard-disk drive numbers 0x80..0x83 for an ext4 partition.
 * First match wins. Returns 0 + writes drive number to *out on success;
 * non-zero on failure. */
static int scan_for_ext4(uint8_t *out_drive) {
    uint8_t d;
    for (d = 0x80; d <= 0x83; d++) {
        if (mount_ext4(d, 1) == 0) {
            *out_drive = d;
            return 0;
        }
    }
    return -1;
}

/* `quiet_fail` suppresses the diagnostic printfs and rolls back the bdev
 * on any failure path — used by scan_for_ext4 when probing 0x80..0x83
 * (most candidates are expected to fail). */
static int mount_ext4(uint8_t drive, int quiet_fail) {
    struct mbr_table mbr;
    int rc;
    int i;

    g_bdev = int13_bdev_open(drive);
    if (!g_bdev) {
        if (!quiet_fail) printf("  int13_bdev_open(0x%02x) failed\n", drive);
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
            if (!quiet_fail) printf("  drive 0x%02x has MBR but no Linux partition\n", drive);
            int13_bdev_close(g_bdev);
            g_bdev = NULL;
            return -2;
        }
    } else if (!quiet_fail) {
        printf("  drive 0x%02x: no MBR; treating as bare ext4\n", drive);
    } else if (rc != 0) {
        /* In a scan, "no MBR" usually means "not a real disk" — bail
         * quickly rather than feeding garbage to ext4_fs_open. */
        int13_bdev_close(g_bdev);
        g_bdev = NULL;
        return -2;
    }

    rc = ext4_fs_open(&g_fs, g_bdev, g_partition_lba);
    if (rc != 0) {
        if (!quiet_fail) printf("  ext4_fs_open failed (rc=%d)\n", rc);
        int13_bdev_close(g_bdev);
        g_bdev = NULL;
        return -3;
    }

    g_fs_ready = 1;

    /* Snapshot critical sb fields to the _DATA-segment safe globals — see
     * the comment at g_safe_block_size. */
    g_safe_block_size           = g_fs.sb.block_size;
    g_safe_blocks_count_lo      = (uint32_t)g_fs.sb.blocks_count;
    g_safe_blocks_count_hi      = (uint32_t)(g_fs.sb.blocks_count >> 32);
    g_safe_free_blocks_count_lo = (uint32_t)g_fs.sb.free_blocks_count;
    g_safe_free_blocks_count_hi = (uint32_t)(g_fs.sb.free_blocks_count >> 32);

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

/* Issue the uninstall probe to a resident TSR and free its memory.
 * Returns 0 on success, non-zero with a printed reason on failure.
 *
 * Sequence: probe to confirm install -> probe AX=11FB to restore the
 * INT 2Fh vector and clear our CDS flag (the resident does this part
 * because vector restore is naturally atomic from inside the handler) ->
 * read the resident's env-block segment from PSP+0x2C -> free both
 * blocks via INT 21h AH=49h. The two AH=49h calls have to happen from
 * outside the resident — we'd be unmapping our own code if we tried to
 * free from the handler. */
static int do_uninstall(void) {
    union REGS  r;
    struct SREGS s;
    uint16_t resident_psp;
    uint16_t env_seg;

    /* Confirm install. */
    r.w.ax = 0x1100u;
    r.w.bx = EXT4_DOS_MAGIC_PROBE;
    int86(0x2F, &r, &r);
    if (r.h.al != 0xFFu || r.w.bx != EXT4_DOS_MAGIC_REPLY) {
        printf("ext4-dos not installed\n");
        return 1;
    }

    /* Tell resident to unhook itself. Returns SI = its PSP segment. */
    r.w.ax = 0x11FBu;
    r.w.bx = EXT4_DOS_MAGIC_PROBE;
    int86(0x2F, &r, &r);
    if (r.h.al != 0xFFu) {
        printf("ext4-dos refused to unhook (another TSR is now on top of\n"
               "the INT 2Fh chain — unload that one first, then retry)\n");
        return 2;
    }
    resident_psp = r.w.si;

    /* Env block segment lives at resident PSP+0x2C (DOS PSP convention).
     * If it's zero, the env was already freed at exec time. */
    env_seg = *(uint16_t __far *)MK_FP(resident_psp, 0x2C);

    /* Free env block first (it may be a separate MCB). */
    if (env_seg != 0u) {
        r.h.ah = 0x49u;
        segread(&s);
        s.es = env_seg;
        intdosx(&r, &r, &s);
        /* Don't fail on env-free errors — env may have been auto-freed. */
    }

    /* Free resident's main MCB (the PSP segment). After this returns,
     * the resident's code/data is officially free DOS memory. */
    r.h.ah = 0x49u;
    segread(&s);
    s.es = resident_psp;
    intdosx(&r, &r, &s);
    if (r.x.cflag) {
        printf("ext4-dos unhooked but freeing resident memory failed "
               "(AX=%04x). Drive Y: is no longer redirected, but the "
               "memory is still allocated. A reboot will clean it up.\n",
               r.w.ax);
        return 3;
    }

    printf("ext4-dos uninstalled\n");
    return 0;
}

int main(int argc, char **argv) {
    uint8_t drive = 0;            /* 0 = unset, scan for one */
    int     drive_specified = 0;
    int     letter_specified = 0;
    char    err[100];
    int     ai;
    int     uninstall = 0;

    /* Parse args: [-q] [-u] [drive_num] [drive_letter:]
     *   -q       : suppress install banner (used by CONFIG.SYS INSTALL=)
     *   -u       : uninstall mode
     *   <num>    : BIOS hard-disk number (e.g. 0x81). If omitted, scan
     *              0x80..0x83 for the first ext4 partition.
     *   <X:>     : drive letter to mount under (e.g. Z:). If omitted,
     *              auto-pick the first free CDS slot (D: or later).
     * Drive letters require the trailing ':' so they don't collide with
     * the bare-letter '-q'/'-u' flags MS-DOS 4 INSTALL= sometimes feeds
     * us with the dash flattened off. */
    for (ai = 1; ai < argc; ai++) {
        const char *a = argv[ai];
        if (a[0] == '-' || a[0] == '/') {
            const char *f = a + 1;
            if      (f[0] == 'q' || f[0] == 'Q') g_quiet = 1;
            else if (f[0] == 'u' || f[0] == 'U') uninstall = 1;
        } else if (a[0] >= '0' && a[0] <= '9') {
            drive = (uint8_t)strtoul(a, NULL, 0);
            drive_specified = 1;
        } else if (((a[0] >= 'A' && a[0] <= 'Z') ||
                    (a[0] >= 'a' && a[0] <= 'z')) && a[1] == ':') {
            uint8_t letter = a[0];
            if (letter >= 'a') letter = (uint8_t)(letter - 0x20);
            g_drive_letter   = letter;
            g_drive_index    = (uint8_t)(letter - 'A');
            letter_specified = 1;
        }
    }

    if (uninstall) {
        return do_uninstall();
    }

    if (!g_quiet) {
        printf("ext4-dos TSR\n");
    }

    /* Pick a drive letter (auto or honor override). Need this BEFORE
     * mount_ext4 so post-mount messages can name the letter. */
    if (!letter_specified) {
        uint8_t letter = pick_drive_letter();
        if (letter == 0) {
            printf("  ERROR: no free CDS slot found. Add LASTDRIVE=Z to\n"
                   "         CONFIG.SYS to give DOS more drive-letter slots,\n"
                   "         or pass an explicit slot like 'EXT4 Z:'.\n");
            return 1;
        }
        g_drive_letter = letter;
        g_drive_index  = (uint8_t)(letter - 'A');
    }

    /* Mount BEFORE hooking the CDS — if mount fails, leave the system
     * untouched rather than installing a no-op redirector that returns
     * "file not found" for every Y: lookup. */
    if (drive_specified) {
        if (mount_ext4(drive, 0) != 0) {
            printf("  ERROR: ext4 mount failed on drive 0x%02x. Refusing to\n"
                   "         install (would leave %c: redirected to nothing).\n",
                   drive, g_drive_letter);
            return 1;
        }
    } else {
        if (scan_for_ext4(&drive) != 0) {
            printf("  ERROR: no ext4 partition found on BIOS drives 0x80..0x83.\n"
                   "         Plug in an ext4 disk, or pass an explicit drive\n"
                   "         number like 'EXT4 0x81' if your BIOS uses a\n"
                   "         non-standard numbering.\n");
            return 1;
        }
        if (!g_quiet) {
            printf("  scanned BIOS drives 0x80..0x83, mounted 0x%02x\n", drive);
        }
    }

    if (ext4_features_check_supported(&g_fs.sb, err, sizeof err) != 0) {
        printf("  ERROR: ext4 mount has unsupported feature: %s\n", err);
        printf("         Refusing to install rather than risk silent misreads.\n");
        return 1;
    }

    if (hook_cds() != 0) {
        return 1;
    }

    g_sda = get_sda();

    if (!g_quiet) {
        printf("  drive %c: marked as redirector (flag 0x%04x)\n",
               g_drive_letter, CDS_FLAG_REDIRECTED);
        printf("  SDA at %04x:%04x\n", FP_SEG(g_sda), FP_OFF(g_sda));
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
