#include "dir.h"
#include "fs.h"
#include "inode.h"
#include "extent.h"
#include "../util/endian.h"
#include <string.h>

int ext4_dir_iter(struct ext4_fs *fs, const struct ext4_inode *dir,
                  ext4_dir_cb cb, void *userdata) {
    static uint8_t blk[EXT4_EXT_NODE_BUF];
    /* Static so &e resolves via DS even when called from a TSR's interrupt
     * handler where SS != DS. The callback reads e via near pointer. */
    static struct ext4_dir_entry e;
    uint32_t bs;
    uint64_t total_size;
    uint32_t blocks;
    uint32_t b;
    uint32_t off;
    int      rc;
    int      r;

    bs = fs->sb.block_size;
    if (bs > sizeof blk) return -1;

    total_size = dir->size;
    blocks = (uint32_t)((total_size + bs - 1u) / bs);

    for (b = 0; b < blocks; b++) {
        rc = ext4_file_read_block(fs, dir, b, blk);
        if (rc) return rc;

        off = 0;
        while (off + 8u <= bs) {
            uint32_t inode_no = le32(blk + off + 0);
            uint16_t rec_len  = le16(blk + off + 4);
            uint8_t  name_len = blk[off + 6];
            uint8_t  file_type = blk[off + 7];

            if (rec_len < 8u || (uint32_t)rec_len > bs - off) break;

            if (inode_no != 0u && name_len > 0u
                    && (uint32_t)name_len + 8u <= rec_len) {
                e.inode     = inode_no;
                e.file_type = file_type;
                e.name_len  = name_len;
                memcpy(e.name, blk + off + 8, name_len);
                e.name[name_len] = '\0';
                r = cb(&e, userdata);
                if (r) return r;
            }
            off += rec_len;
        }
    }
    return 0;
}

struct lookup_state {
    const char *target;
    uint8_t     target_len;
    uint32_t    found_inode;
};

static int lookup_cb(const struct ext4_dir_entry *e, void *ud) {
    struct lookup_state *s = (struct lookup_state *)ud;
    if (e->name_len == s->target_len
            && memcmp(e->name, s->target, s->target_len) == 0) {
        s->found_inode = e->inode;
        return 1;
    }
    return 0;
}

uint32_t ext4_dir_lookup(struct ext4_fs *fs, const struct ext4_inode *dir,
                         const char *name) {
    /* Static so &s resolves via DS, not SS. lookup_cb dereferences s via
     * near pointer — in interrupt-handler context SS != DS. */
    static struct lookup_state s;
    size_t                     n = strlen(name);

    if (n == 0u || n > 255u) return 0u;
    s.target      = name;
    s.target_len  = (uint8_t)n;
    s.found_inode = 0u;
    (void)ext4_dir_iter(fs, dir, lookup_cb, &s);
    return s.found_inode;
}

/* DEBUG: per-step trace bits set during path_lookup so a caller can see
 * which step in the chain failed. Bit 0 = entered, bit 1 = inode_read OK,
 * bit 2 = is-dir check OK, bit 3 = dir_lookup found, bit 4 = returned. */
uint8_t ext4_path_lookup_step;
uint16_t ext4_path_lookup_mode;       /* inode.mode after read */
int16_t ext4_path_lookup_inode_rc;    /* return from ext4_inode_read */
uint32_t ext4_path_lookup_dir_rc;     /* return from ext4_dir_lookup */

uint32_t ext4_path_lookup(struct ext4_fs *fs, const char *path) {
    static struct ext4_inode inode;
    /* Static so the pointer we pass to ext4_dir_lookup resolves via DS. */
    static char    seg[256];
    uint32_t       cur_ino = 2u; /* root */
    const char    *p = path;
    const char    *seg_start;
    size_t         seg_len;
    int            ir;

    ext4_path_lookup_step = 1;
    ext4_path_lookup_mode = 0;
    ext4_path_lookup_inode_rc = 0;
    ext4_path_lookup_dir_rc = 0;

    while (*p == '/') p++;

    while (*p) {
        seg_start = p;
        while (*p && *p != '/') p++;
        seg_len = (size_t)(p - seg_start);
        if (seg_len == 0u) break;
        if (seg_len >= sizeof seg) return 0u;

        memcpy(seg, seg_start, seg_len);
        seg[seg_len] = '\0';

        ir = ext4_inode_read(fs, cur_ino, &inode);
        ext4_path_lookup_inode_rc = (int16_t)ir;
        if (ir != 0) return 0u;
        ext4_path_lookup_step |= 2;
        ext4_path_lookup_mode = inode.mode;

        if ((inode.mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return 0u;
        ext4_path_lookup_step |= 4;

        cur_ino = ext4_dir_lookup(fs, &inode, seg);
        ext4_path_lookup_dir_rc = cur_ino;
        if (cur_ino == 0u) return 0u;
        ext4_path_lookup_step |= 8;

        while (*p == '/') p++;
    }

    ext4_path_lookup_step |= 16;
    return cur_ino;
}
