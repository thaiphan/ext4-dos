#include "journal.h"
#include "fs.h"
#include "inode.h"
#include "extent.h"
#include "../blockdev/blockdev.h"
#include "../util/endian.h"
#include <stdio.h>
#include <string.h>

/* Phase 0 / phase A: read the journal inode + parse the journal
 * superblock. No transaction walking or replay yet. */

static void say(char *err, size_t err_len, const char *msg) {
    if (err && err_len) {
        size_t n = strlen(msg);
        if (n >= err_len) n = err_len - 1u;
        memcpy(err, msg, n);
        err[n] = '\0';
    }
}

int ext4_journal_init(struct ext4_fs *fs, char *err, uint32_t err_len) {
    /* DGROUP statics — DOS small-model has ~4 KB stack, and bdev_read
     * paths can run with SS != DS in interrupt context.
     * See feedback memory: dos_stack_size. */
    static struct ext4_inode jinode;
    static uint8_t           jsb_buf[4096];

    uint32_t inum;
    uint16_t magic, entries, depth;
    int      rc;
    uint32_t i;
    uint64_t first_phys_block;
    uint32_t byte, sector, sectors;
    uint32_t blocktype;
    uint32_t unsupported;

    fs->jbd.present = 0;
    fs->jbd.replay_active = 0;
    fs->jbd.extent_count = 0;
    fs->jbd.replay_count = 0;
    fs->jbd.revoke_count = 0;

    inum = fs->sb.journal_inum;
    if (inum == 0) {
        return 0; /* no journal — clean ext2/ext3-with-no-journal mount */
    }

    rc = ext4_inode_read(fs, inum, &jinode);
    if (rc) {
        snprintf(err, err_len, "journal inode read failed (rc=%d)", rc);
        return -1;
    }

    /* The journal inode's i_block area is an extent header. We only
     * handle depth==0 here — that's how e2fsprogs lays out the journal
     * at mkfs (single contiguous run, or a small number of runs). If
     * we ever see a deeper tree we'd need to recurse, so abort cleanly
     * rather than half-replay. */
    magic   = le16(jinode.i_block + 0);
    entries = le16(jinode.i_block + 2);
    depth   = le16(jinode.i_block + 6);

    if (magic != EXT4_EXT_MAGIC) {
        snprintf(err, err_len, "journal inode not extent-format (magic=0x%x)",
                 (unsigned)magic);
        return -2;
    }
    if (depth != 0) {
        snprintf(err, err_len, "journal extent tree depth %u unsupported",
                 (unsigned)depth);
        return -3;
    }
    if (entries == 0 || entries > EXT4_JBD_EXTENT_MAX) {
        snprintf(err, err_len, "journal extent count %u out of range",
                 (unsigned)entries);
        return -4;
    }

    fs->jbd.extent_count = entries;
    for (i = 0; i < entries; i++) {
        const uint8_t *ext = jinode.i_block + 12u + i * 12u;
        uint16_t real_len = (uint16_t)(le16(ext + 4) & 0x7FFFu);
        fs->jbd.extents[i].logical  = le32(ext + 0);
        fs->jbd.extents[i].length   = real_len;
        fs->jbd.extents[i].physical = ((uint64_t)le16(ext + 6) << 32)
                                    | (uint64_t)le32(ext + 8);
    }

    /* Journal block index 0 is the journal superblock. We require
     * extent[0] to start at logical 0 — anything else means a sparsely
     * laid out journal, which isn't normal. */
    if (fs->jbd.extents[0].logical != 0) {
        snprintf(err, err_len, "journal block 0 missing from extent[0]");
        return -5;
    }
    first_phys_block = fs->jbd.extents[0].physical;

    if (fs->sb.block_size > sizeof jsb_buf) {
        snprintf(err, err_len, "fs block_size %lu > jsb buf",
                 (unsigned long)fs->sb.block_size);
        return -6;
    }
    byte    = (uint32_t)(first_phys_block * (uint64_t)fs->sb.block_size);
    sector  = (uint32_t)(fs->partition_lba + byte / fs->bd->sector_size);
    sectors = fs->sb.block_size / fs->bd->sector_size;
    rc = bdev_read(fs->bd, sector, sectors, jsb_buf);
    if (rc) {
        snprintf(err, err_len, "journal sb read failed (rc=%d)", rc);
        return -7;
    }

    if (be32(jsb_buf + 0x00) != EXT4_JBD_MAGIC) {
        say(err, err_len, "journal superblock bad magic");
        return -8;
    }
    blocktype = be32(jsb_buf + 0x04);
    if (blocktype != EXT4_JBD_BT_SUPERBLOCK && blocktype != EXT4_JBD_BT_SUPER_V2) {
        snprintf(err, err_len, "journal sb blocktype %lu unexpected",
                 (unsigned long)blocktype);
        return -9;
    }

    /* Static fields (offsets per references/lwext4/include/ext4_types.h
     * struct jbd_sb). All multi-byte values are big-endian. */
    fs->jbd.blocksize = be32(jsb_buf + 0x0C);
    fs->jbd.maxlen    = be32(jsb_buf + 0x10);
    fs->jbd.first     = be32(jsb_buf + 0x14);
    fs->jbd.sequence  = be32(jsb_buf + 0x18);
    fs->jbd.start     = be32(jsb_buf + 0x1C);

    if (blocktype == EXT4_JBD_BT_SUPER_V2) {
        fs->jbd.feature_incompat = be32(jsb_buf + 0x28);
        memcpy(fs->jbd.uuid, jsb_buf + 0x30, 16);
    } else {
        fs->jbd.feature_incompat = 0;
        memset(fs->jbd.uuid, 0, 16);
    }

    if (fs->jbd.blocksize != fs->sb.block_size) {
        snprintf(err, err_len, "journal blocksize %lu != fs %lu",
                 (unsigned long)fs->jbd.blocksize,
                 (unsigned long)fs->sb.block_size);
        return -10;
    }

    /* ASYNC_COMMIT changes the on-disk semantics in ways replay would
     * misinterpret. Refuse it. CSUM_V2/V3 we tolerate by ignoring (phase
     * B will verify); REVOKE and 64BIT are normal. */
    unsupported = fs->jbd.feature_incompat & ~((uint32_t)(
        EXT4_JBD_INCOMPAT_REVOKE  |
        EXT4_JBD_INCOMPAT_64BIT   |
        EXT4_JBD_INCOMPAT_CSUM_V2 |
        EXT4_JBD_INCOMPAT_CSUM_V3));
    if (unsupported) {
        snprintf(err, err_len, "journal incompat 0x%lx unsupported",
                 (unsigned long)unsupported);
        return -11;
    }

    fs->jbd.csum_v2   = (uint8_t)((fs->jbd.feature_incompat & EXT4_JBD_INCOMPAT_CSUM_V2) ? 1 : 0);
    fs->jbd.csum_v3   = (uint8_t)((fs->jbd.feature_incompat & EXT4_JBD_INCOMPAT_CSUM_V3) ? 1 : 0);
    fs->jbd.has_64bit = (uint8_t)((fs->jbd.feature_incompat & EXT4_JBD_INCOMPAT_64BIT)   ? 1 : 0);
    fs->jbd.inum      = inum;
    fs->jbd.present   = 1;

    /* replay_active stays 0 in phase A. Phase B will walk the log and
     * set it iff start != 0 and the walk produces a usable map. */
    if (err && err_len) err[0] = '\0';
    return 0;
}
