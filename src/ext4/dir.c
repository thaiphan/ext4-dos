#include "dir.h"
#include "fs.h"
#include "inode.h"
#include "extent.h"
#include "../util/endian.h"
#include <string.h>

int ext4_dir_iter(struct ext4_fs *fs, const struct ext4_inode *dir,
                  ext4_dir_cb cb, void *userdata) {
    static uint8_t blk[EXT4_EXT_NODE_BUF];
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
                struct ext4_dir_entry e;
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
    struct lookup_state s;
    size_t              n = strlen(name);

    if (n == 0u || n > 255u) return 0u;
    s.target      = name;
    s.target_len  = (uint8_t)n;
    s.found_inode = 0u;
    (void)ext4_dir_iter(fs, dir, lookup_cb, &s);
    return s.found_inode;
}

uint32_t ext4_path_lookup(struct ext4_fs *fs, const char *path) {
    static struct ext4_inode inode;
    uint32_t       cur_ino = 2u; /* root */
    const char    *p = path;
    const char    *seg_start;
    size_t         seg_len;
    char           seg[256];

    while (*p == '/') p++;

    while (*p) {
        seg_start = p;
        while (*p && *p != '/') p++;
        seg_len = (size_t)(p - seg_start);
        if (seg_len == 0u) break;
        if (seg_len >= sizeof seg) return 0u;

        memcpy(seg, seg_start, seg_len);
        seg[seg_len] = '\0';

        if (ext4_inode_read(fs, cur_ino, &inode) != 0) return 0u;
        if ((inode.mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return 0u;

        cur_ino = ext4_dir_lookup(fs, &inode, seg);
        if (cur_ino == 0u) return 0u;

        while (*p == '/') p++;
    }

    return cur_ino;
}
