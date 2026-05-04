#include "htree.h"
#include "fs.h"
#include "extent.h"
#include "inode.h"
#include "../util/endian.h"
#include <string.h>

/* Reference: linux fs/ext4/hash.c (the canonical source); cross-checked
 * against lwext4 src/ext4_hash.c. We rewrite rather than vendor.
 * The "half MD4" mixer here implements three rounds (FF/GG/HH) of the
 * standard MD4 transform, deliberately *omitting* the fourth (II) round
 * — Linux did this for speed and "good enough" hash quality in dir-
 * index lookups, and it became part of the on-disk hash semantics.
 * Bit-exact match required for cross-implementation interop. */

static uint32_t F_func(uint32_t x, uint32_t y, uint32_t z) { return (z ^ (x & (y ^ z))); }
static uint32_t G_func(uint32_t x, uint32_t y, uint32_t z) { return ((x & y) + ((x ^ y) & z)); }
static uint32_t H_func(uint32_t x, uint32_t y, uint32_t z) { return (x ^ y ^ z); }

static uint32_t rotl32(uint32_t v, unsigned int n) {
    return (v << n) | (v >> (32u - n));
}

#define FF(a, b, c, d, x, s) do { (a) += F_func((b), (c), (d)) + (x); (a) = rotl32((a), (s)); } while (0)
#define GG(a, b, c, d, x, s) do { (a) += G_func((b), (c), (d)) + (x) + 0x5A827999u; (a) = rotl32((a), (s)); } while (0)
#define HH(a, b, c, d, x, s) do { (a) += H_func((b), (c), (d)) + (x) + 0x6ED9EBA1u; (a) = rotl32((a), (s)); } while (0)

static void half_md4_mix(uint32_t hash[4], const uint32_t data[8]) {
    uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3];

    /* Round 1 */
    FF(a, b, c, d, data[0],  3);
    FF(d, a, b, c, data[1],  7);
    FF(c, d, a, b, data[2], 11);
    FF(b, c, d, a, data[3], 19);
    FF(a, b, c, d, data[4],  3);
    FF(d, a, b, c, data[5],  7);
    FF(c, d, a, b, data[6], 11);
    FF(b, c, d, a, data[7], 19);

    /* Round 2 */
    GG(a, b, c, d, data[1],  3);
    GG(d, a, b, c, data[3],  5);
    GG(c, d, a, b, data[5],  9);
    GG(b, c, d, a, data[7], 13);
    GG(a, b, c, d, data[0],  3);
    GG(d, a, b, c, data[2],  5);
    GG(c, d, a, b, data[4],  9);
    GG(b, c, d, a, data[6], 13);

    /* Round 3 */
    HH(a, b, c, d, data[3],  3);
    HH(d, a, b, c, data[7],  9);
    HH(c, d, a, b, data[2], 11);
    HH(b, c, d, a, data[6], 15);
    HH(a, b, c, d, data[1],  3);
    HH(d, a, b, c, data[5],  9);
    HH(c, d, a, b, data[0], 11);
    HH(b, c, d, a, data[4], 15);

    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
}

/* Pack `slen` source bytes into `dst[0..dlen/4-1]` as 32-bit words,
 * using `slen | slen<<8 | slen<<16 | slen<<24` as the trailing
 * padding. Bit-for-bit match with linux fs/ext4/hash.c's str2hashbuf.
 * `unsigned_char` selects whether bytes are read as unsigned (modern
 * default — HALF_MD4_UNSIGNED) or signed (legacy quirk). */
static void prep_hashbuf(const char *src, uint32_t slen, uint32_t *dst,
                         int dlen, int unsigned_char) {
    uint32_t padding = slen | (slen << 8) | (slen << 16) | (slen << 24);
    uint32_t buf_val = padding;
    int      consume = (slen > (uint32_t)dlen) ? dlen : (int)slen;
    int      i;

    for (i = 0; i < consume; i++) {
        int byte = unsigned_char
                   ? (int)(unsigned char)src[i]
                   : (int)(signed char)  src[i];
        if ((i & 3) == 0) buf_val = padding;
        buf_val = (buf_val << 8) + (uint32_t)byte;
        if ((i & 3) == 3) {
            *dst++ = buf_val;
            dlen   -= 4;
            buf_val = padding;
        }
    }

    /* Trailing word — partial if (i & 3) != 0, else equal to padding. */
    if (dlen >= 4) {
        *dst++ = buf_val;
        dlen   -= 4;
    }
    while (dlen >= 4) {
        *dst++ = padding;
        dlen   -= 4;
    }
}

int ext4_htree_hash_name(uint8_t hash_version, const uint32_t seed[4],
                         const char *name, uint8_t name_len,
                         uint32_t *out_hash_major) {
    /* MD4 IV (per RFC 1320) — overridden by sb->s_hash_seed if non-zero. */
    uint32_t hash[4] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u };
    uint32_t data[8];
    uint32_t major;
    int      unsigned_char = 0;
    int      v             = hash_version;
    int      remaining     = (int)name_len;
    const char *p          = name;

    if (name_len == 0u) return -1;

    /* Non-zero seed overrides the IV. Linux uses raw LE u32 from sb. */
    if (seed[0] || seed[1] || seed[2] || seed[3]) {
        hash[0] = seed[0]; hash[1] = seed[1];
        hash[2] = seed[2]; hash[3] = seed[3];
    }

    /* The *_UNSIGNED variants treat name bytes as unsigned char (fixes
     * a legacy signed-char ambiguity). On little-endian x86/DOS this
     * lines up with what mkfs.ext4 emits when default_hash_version=4
     * (HALF_MD4_UNSIGNED). */
    switch (v) {
    case EXT4_HTREE_HASH_HALF_MD4_UNSIGNED: unsigned_char = 1; v = EXT4_HTREE_HASH_HALF_MD4; break;
    case EXT4_HTREE_HASH_TEA_UNSIGNED:      unsigned_char = 1; v = EXT4_HTREE_HASH_TEA;      break;
    case EXT4_HTREE_HASH_LEGACY_UNSIGNED:   unsigned_char = 1; v = EXT4_HTREE_HASH_LEGACY;   break;
    default: break;
    }

    if (v != EXT4_HTREE_HASH_HALF_MD4) {
        /* TEA and legacy aren't implemented — caller refuses the op. */
        return -1;
    }

    while (remaining > 0) {
        prep_hashbuf(p, (uint32_t)remaining, data, 32, unsigned_char);
        half_md4_mix(hash, data);
        remaining -= 32;
        p         += 32;
    }
    /* Half-MD4: hash[1] is "major", hash[2] is "minor".
     * Major is what the index entries store; we don't need minor here. */
    major = hash[1];

    /* Spec quirk: low bit reserved (collision marker), and the value
     * (HTREE_EOF<<1) is treated as terminal. EOF=0x7FFFFFFF in linux. */
    major &= ~1u;
    if (major == (0x7FFFFFFFu << 1)) major = (0x7FFFFFFFu - 1u) << 1;

    *out_hash_major = major;
    return 0;
}

/* dx_root layout (offsets from start of htree root block):
 *   0x00..0x0B   fake "."  entry (12 bytes incl name)
 *   0x0C..0x17   fake ".." entry (12 bytes incl name; rec_len = bs - 12 hides the index)
 *   0x18..0x1B   reserved_zero  (4 bytes; 0)
 *   0x1C         hash_version    (1 byte; per-dir override of sb default — usually equal)
 *   0x1D         info_length     (1 byte; size of this info block, typically 8)
 *   0x1E         indirect_levels (1 byte; 0 = single-level htree, 1 = two-level, etc.)
 *   0x1F         unused_flags    (1 byte)
 *   0x20         limit  (uint16 LE; max # entries this block can hold incl. e[0])
 *   0x22         count  (uint16 LE; current # entries; >= 1)
 *   0x24         e[0].block (4 bytes; e[0]'s hash slot is occupied by limit_count
 *                            above, so e[0] only stores a block index — used
 *                            as the leaf for hashes below e[1].hash)
 *   0x28..       e[i] for i>=1, each 8 bytes: hash u32 LE + block u32 LE
 *
 * Equivalent unified view: e[i] starts at 0x20 + i*8. e[i].block is at
 * 0x20 + i*8 + 4. e[i].hash for i>=1 is at 0x20 + i*8 (== 0x28 for i=1).
 *
 * Index semantics:
 *   e[0].hash is unused (logically -infinity); e[0].block is the leaf for
 *   hashes < e[1].hash. e[i].block is the leaf for hashes in
 *   [e[i].hash, e[i+1].hash) for 1 <= i < count-1; e[count-1].block
 *   covers [e[count-1].hash, +infinity). Find via:
 *      leaf = e[0].block;
 *      for i in 1..count-1:
 *          if (e[i].hash > our_hash) break;
 *          leaf = e[i].block;
 */
int ext4_htree_find_leaf(struct ext4_fs *fs,
                         const struct ext4_inode *parent,
                         const char *name, uint8_t name_len,
                         uint8_t *scratch,
                         uint32_t *out_leaf_logical) {
    uint32_t        sb_seed[4];
    uint64_t        sb_fs_block;
    uint32_t        sb_offset;
    uint64_t        root_phys;
    uint8_t         hash_version;
    uint8_t         indirect_levels;
    uint16_t        count;
    uint32_t        my_hash;
    uint32_t        i;
    int             rc;

    /* Read the FS superblock first to fetch s_hash_seed (offset 0xEC,
     * 4 u32 LE). Use scratch — it'll get overwritten by the root block
     * read below, but we copy seed out to a 16-byte stack array. */
    sb_fs_block = (fs->sb.block_size > 1024u) ? 0u : 1u;
    sb_offset   = (fs->sb.block_size > 1024u) ? 1024u : 0u;
    rc = ext4_fs_read_block(fs, sb_fs_block, scratch);
    if (rc) return -1;
    sb_seed[0] = le32(scratch + sb_offset + 0xEC);
    sb_seed[1] = le32(scratch + sb_offset + 0xF0);
    sb_seed[2] = le32(scratch + sb_offset + 0xF4);
    sb_seed[3] = le32(scratch + sb_offset + 0xF8);

    /* Now read the root (logical block 0 of the dir) into scratch. */
    rc = ext4_extent_lookup(fs, parent->i_block, 0, &root_phys);
    if (rc) return -1;
    rc = ext4_fs_read_block(fs, root_phys, scratch);
    if (rc) return -1;

    /* Per-dir hash version override. mkfs.ext4 currently writes the same
     * value as sb->def_hash_version, but the spec allows divergence. */
    hash_version    = scratch[0x1C];
    indirect_levels = scratch[0x1E];

    if (indirect_levels > 0u) return -3;  /* multi-level: refuse for now */

    count = le16(scratch + 0x22);
    if (count == 0u) return -4;           /* malformed: empty index */
    if (count * 8u + 0x20u > fs->sb.block_size) return -4;

    rc = ext4_htree_hash_name(hash_version, sb_seed,
                              name, name_len, &my_hash);
    if (rc) return -2;

    /* Linear scan; count is small (at most ~127 in single-level dx_root
     * with 1 KiB blocks). e[i].hash for i>=1 is monotonic-non-decreasing.
     *   e[0].block at offset 0x24 (no hash slot — it overlaps limit_count)
     *   e[i].hash at 0x20 + i*8, e[i].block at 0x24 + i*8 (for i>=1) */
    {
        uint32_t leaf_block = le32(scratch + 0x24);  /* e[0].block */
        for (i = 1; i < (uint32_t)count; i++) {
            uint32_t e_hash = le32(scratch + 0x20u + i*8u);
            if (e_hash > my_hash) break;
            leaf_block = le32(scratch + 0x24u + i*8u);
        }
        *out_leaf_logical = leaf_block;
    }
    return 0;
}
