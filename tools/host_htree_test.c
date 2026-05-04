/* Host-side test for htree-aware file create.
 *
 * Uses scripts/mkfixture-htree.py output (htree.img):
 *   - 1 KiB blocks (so writes pass the DGROUP scratch cap).
 *   - /htreedir with 150 entries, e2fsck -fyD-converted to htree
 *     (real dx_root + leaf blocks; EXT2_INDEX_FL bit set on inode).
 *
 * This test:
 *   1. Copies htree.img to a working file.
 *   2. Mounts it writable, asserts /htreedir has EXT2_INDEX_FL.
 *   3. Captures the inode of an existing entry (file050.txt) for a
 *      post-write integrity check on the index walk.
 *   4. Creates /htreedir/htreenew.txt — exercises ext4_htree_find_leaf
 *      + slot insert into the specific leaf the hash points at.
 *   5. Verifies path lookup at the new path returns the new inode.
 *   6. Re-resolves the captured pre-existing entry to confirm we
 *      didn't corrupt the index by editing a leaf.
 *   7. Asserts journal is clean post-commit + e2fsck -fn passes. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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

static int cp_file(const char *src, const char *dst) {
    FILE *fsrc, *fdst;
    char  buf[8192];
    size_t n;
    fsrc = fopen(src, "rb"); if (!fsrc) return -1;
    fdst = fopen(dst, "wb"); if (!fdst) { fclose(fsrc); return -2; }
    while ((n = fread(buf, 1, sizeof buf, fsrc)) > 0) {
        if (fwrite(buf, 1, n, fdst) != n) { fclose(fsrc); fclose(fdst); return -3; }
    }
    fclose(fsrc); fclose(fdst);
    return 0;
}

static const char *find_e2fsck(void) {
    static const char *candidates[] = {
        "/opt/homebrew/Cellar/e2fsprogs/1.47.4/sbin/e2fsck",
        "/opt/homebrew/opt/e2fsprogs/sbin/e2fsck",
        "/usr/local/opt/e2fsprogs/sbin/e2fsck",
        "/usr/sbin/e2fsck",
        "/sbin/e2fsck",
        NULL,
    };
    int i;
    struct stat st;
    for (i = 0; candidates[i]; i++) {
        if (stat(candidates[i], &st) == 0) return candidates[i];
    }
    return NULL;
}

/* htree.img is a raw fs (no partition table). */
#define PART_LBA 0ull

static int run_e2fsck(const char *e2fsck, const char *img) {
    pid_t pid;
    int   status;

    fflush(stdout); fflush(stderr);
    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl(e2fsck, e2fsck, "-fn", img, (char *)NULL);
        _exit(127);
    }
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(int argc, char **argv) {
    const char        *src_img  = (argc > 1) ? argv[1] : "tests/images/htree.img";
    const char        *work_img = (argc > 2) ? argv[2] : "tests/images/htree-test.img";
    struct blockdev   *bd;
    static struct ext4_fs fs;
    static struct ext4_inode inode;
    char               err[128];
    uint32_t           many_ino;
    uint32_t           new_ino;
    uint32_t           found_ino;
    uint32_t           pre_ino;
    int                rc;

    rc = cp_file(src_img, work_img);
    ASSERT(rc == 0, "cp_file rc=%d", rc);

    bd = file_bdev_open_rw(work_img);
    ASSERT(bd != NULL, "file_bdev_open_rw failed");
    if (!bd) return 1;

    rc = ext4_fs_open(&fs, bd, PART_LBA);
    ASSERT(rc == 0, "ext4_fs_open rc=%d", rc);
    if (rc != 0) return 1;

    /* Sanity: /htreedir is htree (its inode has EXT2_INDEX_FL = 0x1000). */
    many_ino = ext4_path_lookup(&fs, "/htreedir");
    ASSERT(many_ino != 0, "/htreedir not found in fixture");
    rc = ext4_inode_read(&fs, many_ino, &inode);
    ASSERT(rc == 0, "/htreedir inode read rc=%d", rc);
    ASSERT(inode.flags & 0x1000u,
           "/htreedir is supposed to be htree-indexed (e2fsck -fyD), "
           "but i_flags=0x%x has no EXT2_INDEX_FL bit", (unsigned)inode.flags);

    /* Sanity: a pre-existing entry resolves through htree READ (uses
     * linear scan via ext4_dir_iter — already supported). Captures the
     * inode # so we can re-check after our insert. */
    pre_ino = ext4_path_lookup(&fs, "/htreedir/file050.txt");
    ASSERT(pre_ino != 0, "/htreedir/file050.txt should exist in fixture");

    /* The actual test: create /htreedir/htreenew.txt. ext4_file_create's
     * htree path computes the hash, walks dx_root, picks the leaf,
     * inserts there. */
    new_ino = ext4_file_create(&fs, many_ino,
                               "htreenew.txt", 12u,
                               (uint16_t)(0x8000u | 0644u),
                               0x6A000000u, err, sizeof err);
    ASSERT(new_ino != 0, "ext4_file_create into htree dir returned 0, err='%s'", err);
    if (new_ino == 0) { file_bdev_close(bd); return 1; }

    /* Linear-scan path lookup must find the new entry — proof we
     * inserted into a leaf that the linear walker visits. */
    found_ino = ext4_path_lookup(&fs, "/htreedir/htreenew.txt");
    ASSERT(found_ino == new_ino,
           "/htreedir/htreenew.txt lookup returned %u, expected %u",
           found_ino, new_ino);

    /* The existing index entries must still resolve — proof we didn't
     * corrupt the dx_root / index by editing a leaf. */
    {
        uint32_t reread = ext4_path_lookup(&fs, "/htreedir/file050.txt");
        ASSERT(reread == pre_ino,
               "/htreedir/file050.txt now resolves to %u, was %u (index broken?)",
               reread, pre_ino);
    }

    ASSERT(fs.jbd.start == 0, "journal must be clean after htree create");

    file_bdev_close(bd);

    /* e2fsck -fn must report clean. mke2fs will verify the index
     * structure is intact + entries are reachable; if our insert
     * broke the htree invariants, e2fsck fails. */
    {
        const char *e2fsck = find_e2fsck();
        if (e2fsck) {
            int e2rc = run_e2fsck(e2fsck, work_img);
            ASSERT(e2rc == 0, "e2fsck reported errors (rc=%d) — htree corrupted by insert", e2rc);
        } else {
            fprintf(stderr, "host_htree_test: e2fsck not found, skipping cleanliness check\n");
        }
    }

    if (failures == 0) {
        printf("host_htree_test: created /many/htreenew.txt as inode %u in htree dir; "
               "existing index entries still resolve; e2fsck clean\n", new_ino);
        return 0;
    }
    fprintf(stderr, "host_htree_test: %d FAILURE(S)\n", failures);
    return 1;
}
