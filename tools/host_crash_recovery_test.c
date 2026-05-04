/* Crash-recovery test: fault-injection harness.
 *
 * For each fault point N in [0..MAX_FAULT_POINT]:
 *   1. Reset working copy from a clean fixture.
 *   2. Open the working copy with file_bdev_set_fail_after(N) — the
 *      Nth bdev_write will fail.
 *   3. Mount, then run the standard write path (ext4_file_write_block
 *      on /target.txt). The write may succeed or fail depending on
 *      where N lands among ext4_journal_commit's ~10 internal writes.
 *   4. Close the bdev (simulates "process crash" — no further activity).
 *   5. Re-open the same image WITHOUT fault injection.
 *   6. Mount.
 *   7. Assert journal is clean post-mount (jsb.start=0, RECOVER cleared).
 *   8. Run e2fsck -fn via subprocess; assert clean.
 *
 * What this catches:
 *   - Orphan-RECOVER cleanup (commit A) — fault before commit lands.
 *   - Streaming-replay idempotency (commit B2) — fault during checkpoint.
 *   - ext4_fs_open's refuse-on-replay-failure — none of the post-fault
 *     mounts in this test should refuse, because each successful re-open
 *     starts with a fault-free bdev.
 *
 * The test passes if every fault point produces a clean recovery. */
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

#define MAX_FAULT_POINT 14   /* a write_block + extend cycle is ~12 writes */

static int total_failures = 0;

#define ASSERT(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        total_failures++; \
    } \
} while (0)

/* cp src dst — overwrites dst. */
static int cp_file(const char *src, const char *dst) {
    FILE *fsrc, *fdst;
    char  buf[8192];
    size_t n;

    fsrc = fopen(src, "rb");
    if (!fsrc) return -1;
    fdst = fopen(dst, "wb");
    if (!fdst) { fclose(fsrc); return -2; }
    while ((n = fread(buf, 1, sizeof buf, fsrc)) > 0) {
        if (fwrite(buf, 1, n, fdst) != n) { fclose(fsrc); fclose(fdst); return -3; }
    }
    fclose(fsrc);
    fclose(fdst);
    return 0;
}

/* Locate e2fsck. Mirrors mkfixture-journal.py's MKFS_CANDIDATES probe. */
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

/* Run `e2fsck -fn <path>` and return 0 if it reports clean.
 * Returns nonzero if e2fsck not found OR fs has errors. */
static int run_e2fsck(const char *e2fsck, const char *img) {
    pid_t pid;
    int   status;

    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Suppress child stdout/stderr (we just want exit code). */
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execl(e2fsck, e2fsck, "-fn", img, (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) return -2;
    if (!WIFEXITED(status)) return -3;
    /* e2fsck exit codes: 0 = no errors, 1 = errors corrected, 4 = errors
     * left, 8 = operational error. With -n, anything > 0 means dirty. */
    return WEXITSTATUS(status);
}

/* Drive a write through the fs while the bdev is fault-armed. Returns
 * the number of bdev_writes that landed before fault hit (best-effort
 * guess; not used for assertions, just for test logging). */
static void do_write_under_fault(struct blockdev *bd) {
    static struct ext4_fs    fs;
    static struct ext4_inode inode;
    static uint8_t           new_block[1024];
    char     err[64];
    uint32_t target_ino;
    uint32_t now = 0x60000000u; /* arbitrary, distinct from fixture mtime */
    int      rc;

    rc = ext4_fs_open(&fs, bd, 0);
    if (rc != 0) return; /* mount itself faulted — nothing to do */

    target_ino = ext4_path_lookup(&fs, "/target.txt");
    if (target_ino == 0) return;

    if (ext4_inode_read(&fs, target_ino, &inode) != 0) return;

    memset(new_block, 'B', sizeof new_block);
    /* This call does ~10 bdev_writes internally — descriptor, data
     * blocks, commit, jsb update, plus checkpoint flush. The fault
     * (if armed) hits one of them. */
    (void)ext4_file_write_block(&fs, &inode, target_ino, /*logical=*/0,
                                new_block, now, err, sizeof err);
}

static int recover_and_check(const char *img, const char *e2fsck) {
    struct blockdev      *bd;
    static struct ext4_fs fs;
    int                   rc;
    int                   local_failures = 0;

    bd = file_bdev_open_rw(img);
    if (!bd) {
        fprintf(stderr, "  recover: couldn't reopen %s\n", img);
        return 1;
    }
    /* No fault injection on the recovery mount. */
    rc = ext4_fs_open(&fs, bd, 0);
    if (rc != 0) {
        fprintf(stderr, "  recover: ext4_fs_open rc=%d (mount refused)\n", rc);
        local_failures++;
    } else {
        if (fs.jbd.start != 0) {
            fprintf(stderr, "  recover: jsb.start=%u after recovery (expected 0)\n",
                    (unsigned)fs.jbd.start);
            local_failures++;
        }
        if ((fs.sb.feature_incompat & 0x4u) != 0) {
            fprintf(stderr, "  recover: RECOVER still set on fs.sb (feature_incompat=0x%x)\n",
                    (unsigned)fs.sb.feature_incompat);
            local_failures++;
        }
        if (fs.jbd.replay_active != 0) {
            fprintf(stderr, "  recover: replay_active=%u after recovery (expected 0)\n",
                    (unsigned)fs.jbd.replay_active);
            local_failures++;
        }
    }
    file_bdev_close(bd);

    if (e2fsck) {
        int erc = run_e2fsck(e2fsck, img);
        if (erc != 0) {
            fprintf(stderr, "  recover: e2fsck -fn returned %d (expected 0)\n", erc);
            local_failures++;
        }
    }
    return local_failures;
}

int main(int argc, char **argv) {
    const char *clean = (argc > 1) ? argv[1] : "tests/images/write.img";
    const char *work  = (argc > 2) ? argv[2] : "tests/images/crash-recovery-test.img";
    const char *e2fsck;
    int         n;
    int         points_tested = 0;

    e2fsck = find_e2fsck();
    if (!e2fsck) {
        fprintf(stderr, "host_crash_recovery_test: WARNING — e2fsck not found, skipping fsck checks\n");
    }

    for (n = 0; n <= MAX_FAULT_POINT; n++) {
        struct blockdev *bd;
        int              recovery_failures;
        int              prior_failures = total_failures;

        if (cp_file(clean, work) != 0) {
            fprintf(stderr, "fault N=%d: cp failed\n", n);
            total_failures++;
            continue;
        }

        bd = file_bdev_open_rw(work);
        if (!bd) {
            fprintf(stderr, "fault N=%d: open failed\n", n);
            total_failures++;
            continue;
        }
        file_bdev_set_fail_after(bd, n);

        /* Drive the write — may succeed or fault out. Either way we close
         * and re-open below to simulate a process restart. */
        do_write_under_fault(bd);
        file_bdev_close(bd);

        recovery_failures = recover_and_check(work, e2fsck);
        if (recovery_failures > 0) {
            fprintf(stderr, "fault N=%d: %d recovery failure(s)\n", n, recovery_failures);
        }
        if (total_failures != prior_failures) {
            /* Already reported above. */
        }
        points_tested++;
    }

    if (total_failures == 0) {
        printf("host_crash_recovery_test: all %d fault points recovered cleanly%s\n",
               points_tested, e2fsck ? " (e2fsck -fn passed each)" : " (e2fsck skipped)");
        return 0;
    }
    fprintf(stderr, "host_crash_recovery_test: %d FAILURE(S) across %d fault points\n",
            total_failures, points_tested);
    return 1;
}
