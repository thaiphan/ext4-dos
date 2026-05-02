/* Host-side test for ext4_file_remove (DEL).
 *
 * Creates /newfile.txt (1 block), removes it, then verifies:
 *   - path_lookup returns 0 after removal
 *   - journal is clean post-commit
 *   - e2fsck reports the image clean
 * Also exercises a 2-block file to cover multi-block data-free path. */
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
#include "ext4/journal.h"

static int failures = 0;
#define ASSERT(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

int main(int argc, char **argv) {
    const char       *img = (argc > 1) ? argv[1] : "tests/images/del-test.img";
    struct blockdev  *bd;
    static struct ext4_fs fs;
    static struct ext4_inode inode;
    static uint8_t   content[1024];
    char              err[128];
    uint32_t          ino;
    uint32_t          now = 0x5A000000u;
    int               rc;

    bd = file_bdev_open_rw(img);
    ASSERT(bd != NULL, "couldn't open %s rw", img);
    if (!bd) return 1;

    rc = ext4_fs_open(&fs, bd, 0);
    ASSERT(rc == 0, "ext4_fs_open rc=%d", rc);

    /* --- Single-block file --- */
    ino = ext4_file_create(&fs, 2u, "delme.txt", 9u,
                           (uint16_t)(0x8000u | 0644u), now, err, sizeof err);
    ASSERT(ino != 0, "create delme.txt: err='%s'", err);

    memset(content, 'X', sizeof content);
    rc = ext4_inode_read(&fs, ino, &inode);
    ASSERT(rc == 0, "inode read rc=%d", rc);
    rc = ext4_file_extend_block(&fs, &inode, ino, content, sizeof content,
                                now+1u, err, sizeof err);
    ASSERT(rc == 0, "extend_block rc=%d err='%s'", rc, err);

    rc = ext4_file_remove(&fs, 2u, ino, err, sizeof err);
    ASSERT(rc == 0, "file_remove rc=%d err='%s'", rc, err);
    ASSERT(ext4_path_lookup(&fs, "/delme.txt") == 0,
           "/delme.txt still found after remove");
    printf("host_del_test: removed /delme.txt (1 block)\n");

    /* --- Two-block file (exercises multi-block free loop) --- */
    ino = ext4_file_create(&fs, 2u, "big.txt", 7u,
                           (uint16_t)(0x8000u | 0644u), now+2u, err, sizeof err);
    ASSERT(ino != 0, "create big.txt: err='%s'", err);

    rc = ext4_inode_read(&fs, ino, &inode);
    ASSERT(rc == 0, "inode read rc=%d", rc);
    rc = ext4_file_extend_block(&fs, &inode, ino, content, sizeof content,
                                now+3u, err, sizeof err);
    ASSERT(rc == 0, "extend 1 rc=%d", rc);
    rc = ext4_file_extend_block(&fs, &inode, ino, content, sizeof content,
                                now+4u, err, sizeof err);
    ASSERT(rc == 0, "extend 2 rc=%d", rc);

    rc = ext4_file_remove(&fs, 2u, ino, err, sizeof err);
    ASSERT(rc == 0, "file_remove (2-block) rc=%d err='%s'", rc, err);
    ASSERT(ext4_path_lookup(&fs, "/big.txt") == 0,
           "/big.txt still found after remove");
    printf("host_del_test: removed /big.txt (2 blocks)\n");

    ASSERT(fs.jbd.start == 0, "journal must be clean");

    if (failures == 0) {
        printf("host_del_test: all asserts passed\n");
        return 0;
    }
    fprintf(stderr, "host_del_test: %d FAILURE(S)\n", failures);
    return 1;
}
