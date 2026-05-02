/* Host-side stress test for ext4-dos. Mounts the stress fixture (256 MiB
 * ext4 with default mkfs.ext4 features at 4 KiB blocks), exercises:
 *
 *   - Path walk, multiple components deep (/a/b/c/deep.txt)
 *   - htree directory lookup (/many/file{0..499}.txt — 500 files)
 *   - Multi-block read across multiple extents (/big.bin, 4 MiB)
 *
 * The 4 MiB read is verified against a SHA-256 hash baked into the
 * fixture at install time (/big.sha256). Drift in the fixture itself
 * is caught — Python writes the hash, this binary recomputes it on the
 * bytes that came back through our extent reader.
 *
 * Run as part of `make host-stress`. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "blockdev/blockdev.h"
#include "blockdev/file_bdev.h"
#include "ext4/superblock.h"
#include "ext4/features.h"
#include "ext4/fs.h"
#include "ext4/inode.h"
#include "ext4/extent.h"
#include "ext4/dir.h"
#include "partition/mbr.h"

/* Small-but-enough SHA-256 implementation. Self-contained so the test
 * doesn't pull in OpenSSL on a headless CI box. */
typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    size_t   buflen;
} sha256_ctx;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static void sha256_init(sha256_ctx *c) {
    static const uint32_t H0[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(c->h, H0, sizeof H0);
    c->len = 0;
    c->buflen = 0;
}
static void sha256_block(sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64], a, b, d, e, f, g, hh, t1, t2, ch, mj, s0, s1, ee, ff;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16)
             | ((uint32_t)p[i*4+2] <<  8) | ((uint32_t)p[i*4+3]);
    }
    for (i = 16; i < 64; i++) {
        s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19)  ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->h[0]; b = c->h[1]; uint32_t cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; hh = c->h[7];
    for (i = 0; i < 64; i++) {
        ee = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        ch = (e & f) ^ (~e & g);
        t1 = hh + ee + ch + K256[i] + w[i];
        ff = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        mj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = ff + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=hh;
}
static void sha256_update(sha256_ctx *c, const void *data, size_t n) {
    const uint8_t *p = data;
    c->len += n;
    if (c->buflen) {
        size_t fill = 64 - c->buflen;
        if (fill > n) fill = n;
        memcpy(c->buf + c->buflen, p, fill);
        c->buflen += fill; p += fill; n -= fill;
        if (c->buflen == 64) { sha256_block(c, c->buf); c->buflen = 0; }
    }
    while (n >= 64) { sha256_block(c, p); p += 64; n -= 64; }
    if (n) { memcpy(c->buf, p, n); c->buflen = n; }
}
static void sha256_final(sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    int i;
    sha256_update(c, &pad, 1);
    pad = 0;
    while (c->buflen != 56) sha256_update(c, &pad, 1);
    for (i = 7; i >= 0; i--) { uint8_t b = (bits >> (i*8)) & 0xff; sha256_update(c, &b, 1); }
    for (i = 0; i < 8; i++) {
        out[i*4]   = c->h[i] >> 24;
        out[i*4+1] = c->h[i] >> 16;
        out[i*4+2] = c->h[i] >> 8;
        out[i*4+3] = c->h[i];
    }
}

/* ----- Test harness ------------------------------------------------------- */

static int failures = 0;
#define ASSERT(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

static int read_full_file(struct ext4_fs *fs, uint32_t ino, void *out, uint32_t cap, uint32_t *got) {
    struct ext4_inode inode;
    int rc = ext4_inode_read(fs, ino, &inode);
    if (rc) return rc;
    uint32_t want = (uint32_t)inode.size;
    if (want > cap) return -100;
    int actual = ext4_file_read_head(fs, &inode, want, out);
    if (actual < 0) return actual;
    *got = (uint32_t)actual;
    return 0;
}

int main(int argc, char **argv) {
    const char *path = (argc >= 2) ? argv[1] : "tests/images/stress.img";
    struct blockdev *bd;
    struct mbr_table mbr;
    static struct ext4_fs fs;
    char err[128];
    uint32_t ino;
    static uint8_t big_buf[4 * 1024 * 1024 + 16];
    static uint8_t expected_hex[80];
    uint32_t got;
    int rc;

    bd = file_bdev_open(path);
    ASSERT(bd != NULL, "couldn't open %s", path);
    if (!bd) return 1;

    rc = mbr_read(bd, &mbr);
    ASSERT(rc == 0, "mbr_read rc=%d", rc);
    uint64_t part_lba = 0;
    for (int i = 0; i < mbr.count; i++) {
        if (mbr.entries[i].type == MBR_TYPE_LINUX) {
            part_lba = mbr.entries[i].start_lba;
            break;
        }
    }
    ASSERT(part_lba != 0, "no Linux partition in MBR");

    rc = ext4_fs_open(&fs, bd, part_lba);
    ASSERT(rc == 0, "ext4_fs_open rc=%d", rc);

    rc = ext4_features_check_supported(&fs.sb, err, sizeof err);
    ASSERT(rc == 0, "feature-flag refused supported fixture: %s", err);

    /* Phase A journal sanity: stress fixture is built with default
     * mkfs.ext4, so it has a journal. Parse must succeed and the log
     * must be clean (start == 0 for a freshly-created FS). */
    ASSERT(fs.jbd.present == 1, "journal expected present (sb.journal_inum=%u)",
           (unsigned)fs.sb.journal_inum);
    ASSERT(fs.jbd.blocksize == fs.sb.block_size,
           "journal blocksize %u != fs %u",
           (unsigned)fs.jbd.blocksize, (unsigned)fs.sb.block_size);
    ASSERT(fs.jbd.maxlen > 0, "journal maxlen must be > 0");
    ASSERT(fs.jbd.start == 0, "fresh fixture: journal must be clean (start=%u)",
           (unsigned)fs.jbd.start);

    /* /readme.txt */
    ino = ext4_path_lookup(&fs, "/readme.txt");
    ASSERT(ino != 0, "/readme.txt not found");

    /* Nested path: /a/b/c/deep.txt */
    ino = ext4_path_lookup(&fs, "/a/b/c/deep.txt");
    ASSERT(ino != 0, "/a/b/c/deep.txt not found");

    /* htree directory: spot-check 100 files in /many */
    int many_misses = 0;
    char buf[64];
    for (int i = 0; i < 500; i += 5) {
        snprintf(buf, sizeof buf, "/many/file%04d.txt", i);
        if (ext4_path_lookup(&fs, buf) == 0) many_misses++;
    }
    ASSERT(many_misses == 0, "htree spot-check missed %d files in /many", many_misses);

    /* Multi-block read: /big.bin (4 MiB), verify SHA-256 matches /big.sha256 */
    ino = ext4_path_lookup(&fs, "/big.bin");
    ASSERT(ino != 0, "/big.bin not found");
    if (ino) {
        rc = read_full_file(&fs, ino, big_buf, sizeof big_buf, &got);
        ASSERT(rc == 0, "read /big.bin rc=%d", rc);
        ASSERT(got == 4 * 1024 * 1024, "expected 4 MiB, got %u bytes", got);

        sha256_ctx ctx;
        uint8_t digest[32];
        sha256_init(&ctx);
        sha256_update(&ctx, big_buf, got);
        sha256_final(&ctx, digest);

        char actual_hex[80];
        for (int i = 0; i < 32; i++) snprintf(actual_hex + i*2, 4, "%02x", digest[i]);
        actual_hex[64] = '\0';

        /* Read expected hash. */
        uint32_t sha_ino = ext4_path_lookup(&fs, "/big.sha256");
        ASSERT(sha_ino != 0, "/big.sha256 not found");
        if (sha_ino) {
            rc = read_full_file(&fs, sha_ino, expected_hex, sizeof expected_hex, &got);
            ASSERT(rc == 0, "read /big.sha256 rc=%d", rc);
            expected_hex[64] = '\0';
            ASSERT(strcmp((char *)expected_hex, actual_hex) == 0,
                   "SHA-256 mismatch: expected %s, got %s",
                   expected_hex, actual_hex);
        }
    }

    if (failures == 0) {
        printf("host_stress_test: all asserts passed\n");
        return 0;
    }
    fprintf(stderr, "host_stress_test: %d FAILURE(S)\n", failures);
    return 1;
}
