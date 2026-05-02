#include <stdio.h>
#include <stdint.h>
#include <ctype.h>
#include <dos.h>
#include <i86.h>

#define EXT4_DOS_MAGIC_PROBE 0xE4D0u

struct ff_capture {
    uint8_t  valid;
    uint16_t ax, bx, cx, dx;
    uint16_t si, di, bp;
    uint16_t ds, es, flags;
    uint8_t  sda_bytes[256];
    uint8_t  es_di_bytes[64];
    uint8_t  ds_si_bytes[64];
    uint16_t flow_entered;
    uint16_t flow_inode_root_fail;
    int16_t  flow_inode_root_rc;
    uint16_t flow_dir_iter_returned;
    uint16_t flow_state_not_found;
    uint16_t flow_inode_entry_fail;
    uint16_t flow_success;
    uint8_t  fs_ready_at_call;
    uint32_t fs_bgd_count;
    uint16_t fs_bgd_size;
    uint32_t fs_blocks_per_group;
    uint32_t fs_inodes_per_group;
    uint16_t fs_inode_size;
    uint32_t fs_block_size;
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
#pragma pack(push, 1)
    struct {
        uint32_t inode_num;
        uint8_t  step;
        uint16_t mode;
        int16_t  inode_rc;
        uint32_t dir_rc;
    } open_calls[2];
#pragma pack(pop)
    uint8_t  open_call_idx;
    uint32_t entry_inode_mtime;
    uint16_t entry_inode_dos_time;
    uint16_t entry_inode_dos_date;
    uint32_t utd_initial_days;
    uint16_t utd_year_iters;
    uint32_t utd_days_after_year_loop;
    uint16_t utd_final_year;
};

static void hex_dump(const char *label, const uint8_t __far *p, unsigned len) {
    unsigned i, j;
    printf("%s (%u bytes):\n", label, len);
    for (i = 0; i < len; i += 16) {
        printf("  %03x: ", i);
        for (j = 0; j < 16 && i + j < len; j++) {
            printf("%02x ", p[i + j]);
        }
        for (; j < 16; j++) printf("   ");
        printf(" |");
        for (j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = p[i + j];
            putchar(isprint(c) ? (char)c : '.');
        }
        printf("|\n");
    }
}

int main(void) {
    union REGS r;
    struct ff_capture __far *cap;

    r.w.ax = 0x11FCu;
    r.w.bx = EXT4_DOS_MAGIC_PROBE;
    int86(0x2F, &r, &r);
    if (r.h.al != 0xFFu) {
        printf("capture probe failed: AL=0x%02x\n", r.h.al);
        return 1;
    }

    cap = (struct ff_capture __far *)MK_FP(r.w.dx, r.w.cx);
    printf("capture buffer at %04x:%04x\n", r.w.dx, r.w.cx);

    /* Always print Open/Read state even if FindFirst capture is empty —
     * MS-DOS 4 dispatches FindFirst as AL=0x19 (IFS_SEQ_SEARCH_FIRST)
     * which doesn't trip the AL=0x1B-only valid flag. */
    {
        unsigned j;
        printf("\nOpen/Read/Close diagnostics (always-on):\n");
        printf("  Open calls    : %u\n", cap->open_call_count);
        printf("  Read calls    : %u\n", cap->read_call_count);
        printf("  Close calls   : %u\n", cap->close_call_count);
        printf("  last open rc  : %d\n", (int)cap->last_open_rc);
        printf("  last open path: '");
        for (j = 0; j < 64 && cap->last_open_path[j]; j++) {
            uint8_t c = cap->last_open_path[j];
            putchar((c >= 0x20 && c < 0x7F) ? c : '.');
        }
        printf("'\n");
        printf("  last open inode = %lu, size = %lu\n",
               (unsigned long)cap->last_open_inode_num,
               (unsigned long)cap->last_open_size);
        printf("  open_call_idx: %u\n", (unsigned)cap->open_call_idx);
        for (j = 0; j < 2; j++) {
            printf("  call[%u]: inode=%lu step=0x%02x mode=0x%04x inode_rc=%d dir_rc=%lu\n",
                   j,
                   (unsigned long)cap->open_calls[j].inode_num,
                   (unsigned)cap->open_calls[j].step,
                   (unsigned)cap->open_calls[j].mode,
                   (int)cap->open_calls[j].inode_rc,
                   (unsigned long)cap->open_calls[j].dir_rc);
        }
    }

    if (!cap->valid) {
        printf("(FindFirst capture skipped — valid=0, only Open data above)\n");
        return 0;
    }

    printf("FindFirst entry registers:\n");
    printf("  AX=%04x BX=%04x CX=%04x DX=%04x  flags=%04x\n",
           cap->ax, cap->bx, cap->cx, cap->dx, cap->flags);
    printf("  SI=%04x DI=%04x BP=%04x  DS=%04x ES=%04x\n",
           cap->si, cap->di, cap->bp, cap->ds, cap->es);

    hex_dump("SDA[0..255]", cap->sda_bytes, 256);
    hex_dump("ES:DI[0..63] (likely DTA)", cap->es_di_bytes, 64);
    hex_dump("DS:SI[0..63] (likely pattern)", cap->ds_si_bytes, 64);

    printf("\nFindFirst flow counters:\n");
    printf("  entered            : %u\n", cap->flow_entered);
    printf("  inode_read root    : rc=%d, fail count %u\n",
           (int)cap->flow_inode_root_rc, cap->flow_inode_root_fail);
    printf("  dir_iter rc        : %u\n", cap->flow_dir_iter_returned);
    printf("  state.found = 0    : %u times\n", cap->flow_state_not_found);
    printf("  inode_read entry   : %s (fail count %u)\n",
           cap->flow_inode_entry_fail ? "FAIL" : "ok", cap->flow_inode_entry_fail);
    printf("  success            : %u\n", cap->flow_success);

    printf("\ng_fs state snapshot at first FindFirst:\n");
    printf("  g_fs_ready         : %u\n", cap->fs_ready_at_call);
    printf("  bgd_count          : %lu\n", (unsigned long)cap->fs_bgd_count);
    printf("  bgd_size           : %u\n", cap->fs_bgd_size);
    printf("  blocks_per_group   : %lu\n", (unsigned long)cap->fs_blocks_per_group);
    printf("  inodes_per_group   : %lu\n", (unsigned long)cap->fs_inodes_per_group);
    printf("  inode_size         : %u\n", cap->fs_inode_size);
    printf("  block_size         : %lu\n", (unsigned long)cap->fs_block_size);

    hex_dump("\nroot_inode.i_block first 32 bytes (header + first extent)",
             cap->root_iblock, 32);

    printf("\nDirect probe of root data block 0:\n");
    printf("  extent_lookup rc   : %d\n", (int)cap->ext_lookup_rc);
    printf("  phys block         : %lu (hi=%lu)\n",
           (unsigned long)cap->ext_lookup_phys_lo,
           (unsigned long)cap->ext_lookup_phys_hi);
    printf("  computed sector    : %lu (0x%08lx)\n",
           (unsigned long)cap->data_sector_lo, (unsigned long)cap->data_sector_lo);
    printf("  partition_lba      : lo=%lu hi=%lu\n",
           (unsigned long)cap->cap_partition_lba_lo,
           (unsigned long)cap->cap_partition_lba_hi);
    printf("  byte_off           : lo=%lu hi=%lu (0x%08lx %08lx)\n",
           (unsigned long)cap->cap_byte_off_lo,
           (unsigned long)cap->cap_byte_off_hi,
           (unsigned long)cap->cap_byte_off_hi,
           (unsigned long)cap->cap_byte_off_lo);
    printf("  bd->sector_size    : %u\n", cap->cap_sector_size);
    printf("  bdev_read rc       : %d\n", (int)cap->data_bdev_read_rc);

    {
        int i;
        printf("\nPer-call FindFirst snapshots:\n");
        for (i = 0; i < 4; i++) {
            if (!cap->calls[i].used) continue;
            printf("  call %d:\n", i + 1);
            printf("    target_index = %u\n", cap->calls[i].target_index);
            printf("    sattr        = 0x%02x\n", cap->calls[i].sattr);
            printf("    rc           = %d\n", (int)cap->calls[i].rc);
            printf("    current_idx  = %u\n", cap->calls[i].current_index_after);
            printf("    state.found  = %u\n", cap->calls[i].state_found);
            if (cap->calls[i].state_found) {
                int j;
                int n = (int)(unsigned char)cap->calls[i].name_len;
                if (n > 16) n = 16;
                printf("    name_len     = %u\n", (unsigned)(unsigned char)cap->calls[i].name_len);
                printf("    name bytes   =");
                for (j = 0; j < 16; j++)
                    printf(" %02x", (unsigned char)cap->calls[i].name[j]);
                printf("\n    name ascii   = '");
                for (j = 0; j < n; j++) {
                    unsigned char c = (unsigned char)cap->calls[i].name[j];
                    putchar(isprint(c) ? (char)c : '?');
                }
                printf("'\n");
            }
            printf("    pri_path     = '");
            {
                int j;
                for (j = 0; j < 80; j++) {
                    uint8_t c = cap->calls[i].pri_path[j];
                    if (c == 0) break;
                    putchar(isprint(c) ? (char)c : '?');
                }
                printf("'\n");
            }
            if (cap->calls[i].rc == 0) {
                int j;
                printf("    name83[11]   =");
                for (j = 0; j < 11; j++)
                    printf(" %02x", (unsigned char)cap->calls[i].name83[j]);
                printf("  '");
                for (j = 0; j < 11; j++) {
                    uint8_t c = cap->calls[i].name83[j];
                    putchar(isprint(c) ? (char)c : '.');
                }
                printf("'\n");
                printf("    SearchDir    =");
                for (j = 0; j < 16; j++)
                    printf(" %02x", (unsigned char)cap->calls[i].searchdir_after[j]);
                printf("\n                  ");
                for (j = 16; j < 32; j++)
                    printf(" %02x", (unsigned char)cap->calls[i].searchdir_after[j]);
                printf("\n");
            }
        }
    }

    printf("\nOpen/Read/Close diagnostics:\n");
    printf("  Open calls    : %u\n", cap->open_call_count);
    printf("  Read calls    : %u\n", cap->read_call_count);
    printf("  Close calls   : %u\n", cap->close_call_count);
    printf("  last open rc  : %d\n", (int)cap->last_open_rc);
    printf("  last open path: '");
    {
        int j;
        for (j = 0; j < 64; j++) {
            uint8_t c = cap->last_open_path[j];
            if (c == 0) break;
            putchar(isprint(c) ? (char)c : '?');
        }
    }
    printf("'\n");
    printf("  last open inode = %lu, size = %lu\n",
           (unsigned long)cap->last_open_inode_num,
           (unsigned long)cap->last_open_size);
    printf("  last read pos = %lu, requested = %u, actual = %d\n",
           (unsigned long)cap->last_read_pos,
           cap->last_read_count, (int)cap->last_read_actual);

    printf("\nEntry-1 mtime conversion:\n");
    printf("  raw mtime    = 0x%08lx (%lu)\n",
           (unsigned long)cap->entry_inode_mtime,
           (unsigned long)cap->entry_inode_mtime);
    printf("  DOS time     = 0x%04x\n", cap->entry_inode_dos_time);
    printf("  DOS date     = 0x%04x  (year=%u, month=%u, day=%u)\n",
           cap->entry_inode_dos_date,
           (unsigned)((cap->entry_inode_dos_date >> 9) + 1980u),
           (unsigned)((cap->entry_inode_dos_date >> 5) & 0x0Fu),
           (unsigned)(cap->entry_inode_dos_date & 0x1Fu));
    printf("  utd initial days   = %lu\n", (unsigned long)cap->utd_initial_days);
    printf("  utd year iters     = %u\n", cap->utd_year_iters);
    printf("  utd days after loop= %lu\n", (unsigned long)cap->utd_days_after_year_loop);
    printf("  utd final year     = %u\n", cap->utd_final_year);

    return 0;
}
