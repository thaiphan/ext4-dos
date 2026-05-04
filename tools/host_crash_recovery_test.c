/* Crash-recovery test: fault-injection across multiple write scenarios.
 *
 * For each scenario × fault point N:
 *   1. Reset working copy from a clean fixture.
 *   2. Optionally run a clean preamble write (multi-transaction case).
 *   3. Open with fault-after-N armed; the (N+1)-th bdev_write fails.
 *   4. Drive the scenario's write — may succeed or fault out partway.
 *   5. Close the bdev (simulates "process crash").
 *   6. Re-open WITHOUT faults; mount triggers ext4_journal_init →
 *      replay → checkpoint.
 *   7. Snapshot the pre-replay state from fs->jbd (RECOVER bit on FS sb,
 *      jsb.start) and bin the fault into a phase class.
 *   8. Assert post-recovery state is clean.
 *   9. Run e2fsck -fn; assert clean.
 *  10. Assert file content matches either the pre- or post-write image
 *      (never a torn mix).
 *
 * Phase classification:
 *   - PRE_RECOVER:  fault before step-1 SB-set-RECOVER landed.
 *   - ORPHAN_RECOVER: RECOVER set on disk but jsb.start=0 — commit
 *                   sequence faulted before step-5 jsb update, OR after
 *                   step-7b jsb-clean but before step-7c RECOVER-clear.
 *   - JSB_DIRTY:    RECOVER set AND jsb.start != 0 — real journal-replay
 *                   path is exercised (mid-checkpoint or post-jsb-update).
 *
 * The test asserts every scenario is clean, AND that across all
 * scenarios we exercised every phase class at least once. */
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

/* Fault-point ranges. The single-block in-place commit is ~10 internal
 * writes; the 5-block extend commit is ~16. Cover both with margin. */
#define MAX_FAULT_POINT_INPLACE  14
#define MAX_FAULT_POINT_EXTEND   20

#define BLOCK_SIZE 1024u  /* matches mkfixture-write.py */

enum phase_class {
    PHASE_PRE_RECOVER = 0,
    PHASE_ORPHAN_RECOVER,
    PHASE_JSB_DIRTY,
    PHASE_COUNT
};

static const char *phase_name(enum phase_class p) {
    switch (p) {
    case PHASE_PRE_RECOVER:    return "pre-RECOVER";
    case PHASE_ORPHAN_RECOVER: return "orphan-RECOVER";
    case PHASE_JSB_DIRTY:      return "jsb-dirty";
    default:                   return "?";
    }
}

static int total_failures = 0;
static int phase_hits_global[PHASE_COUNT] = {0};

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

static int run_e2fsck(const char *e2fsck, const char *img) {
    pid_t pid;
    int   status;

    /* Flush buffered stdio before fork, otherwise the child inherits the
     * pending buffer and freopen()'s flush-on-redirect re-emits the
     * parent's already-printed text on stdout. */
    fflush(stdout);
    fflush(stderr);

    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execl(e2fsck, e2fsck, "-fn", img, (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) return -2;
    if (!WIFEXITED(status)) return -3;
    return WEXITSTATUS(status);
}

/* Drive an in-place write_block on /target.txt, filling logical=0 with
 * `fill_byte` (BLOCK_SIZE bytes). bdev may be fault-armed or not. */
static int do_inplace_write(struct blockdev *bd, uint8_t fill_byte) {
    static struct ext4_fs    fs;
    static struct ext4_inode inode;
    static uint8_t           new_block[BLOCK_SIZE];
    char     err[64];
    uint32_t target_ino;
    uint32_t now = 0x60000000u;
    int      rc;

    rc = ext4_fs_open(&fs, bd, 0);
    if (rc != 0) return rc;

    target_ino = ext4_path_lookup(&fs, "/target.txt");
    if (target_ino == 0) return -100;
    if (ext4_inode_read(&fs, target_ino, &inode) != 0) return -101;

    memset(new_block, fill_byte, sizeof new_block);
    return ext4_file_write_block(&fs, &inode, target_ino, /*logical=*/0,
                                 new_block, now, err, sizeof err);
}

/* Drive an extend on /target.txt — appends one BLOCK_SIZE block of
 * `fill_byte`, growing the file from 1 block to 2 blocks. Builds a
 * 5-trans-block journal commit (data + bitmap + bgd + sb + inode). */
static int do_extend_write(struct blockdev *bd, uint8_t fill_byte) {
    static struct ext4_fs    fs;
    static struct ext4_inode inode;
    static uint8_t           new_block[BLOCK_SIZE];
    char     err[64];
    uint32_t target_ino;
    uint32_t now = 0x60000001u;
    int      rc;

    rc = ext4_fs_open(&fs, bd, 0);
    if (rc != 0) return rc;

    target_ino = ext4_path_lookup(&fs, "/target.txt");
    if (target_ino == 0) return -100;
    if (ext4_inode_read(&fs, target_ino, &inode) != 0) return -101;

    memset(new_block, fill_byte, sizeof new_block);
    return ext4_file_extend_block(&fs, &inode, target_ino,
                                  new_block, BLOCK_SIZE, now, err, sizeof err);
}

/* Re-open without faults, mount, capture pre-replay class, assert
 * post-recovery is clean, optionally read /target.txt's logical=0
 * content, run e2fsck. Returns 1 if any check failed. */
struct recovery_result {
    enum phase_class phase;
    uint8_t          have_block;
    uint8_t          block[BLOCK_SIZE];
    uint64_t         file_size;
};

static int recover_and_check(const char *img, const char *e2fsck,
                             const char *sc_name, int n,
                             struct recovery_result *out) {
    struct blockdev      *bd;
    static struct ext4_fs fs;
    static struct ext4_inode inode;
    int                   local_failures = 0;
    int                   rc;
    uint32_t              target_ino;

    out->phase      = PHASE_PRE_RECOVER;
    out->have_block = 0;
    out->file_size  = 0;

    bd = file_bdev_open_rw(img);
    if (!bd) {
        fprintf(stderr, "  recover: couldn't reopen %s\n", img);
        return 1;
    }
    rc = ext4_fs_open(&fs, bd, 0);
    if (rc != 0) {
        fprintf(stderr, "  [%s/N=%d] recover: ext4_fs_open rc=%d (mount refused)\n",
                sc_name, n, rc);
        local_failures++;
        file_bdev_close(bd);
        return local_failures;
    }

    /* Phase classification — fields are set in ext4_journal_init from the
     * on-disk state, before any replay or checkpoint touches it.
     *   - jsb.start != 0 means real replay walker work, regardless of
     *     RECOVER bit (a journaled FS SB flushed during checkpoint can
     *     leave RECOVER=0 even though the txn isn't yet jsb-clean).
     *   - jsb.start == 0 with RECOVER set is the orphan-RECOVER case.
     *   - jsb.start == 0 with RECOVER clear is pre-step-1 (no work). */
    if (fs.jbd.pre_replay_start != 0) {
        out->phase = PHASE_JSB_DIRTY;
    } else if (fs.jbd.pre_replay_recover) {
        out->phase = PHASE_ORPHAN_RECOVER;
    } else {
        out->phase = PHASE_PRE_RECOVER;
    }

    if (fs.jbd.start != 0) {
        fprintf(stderr, "  [%s/N=%d] recover: jsb.start=%u after recovery (expected 0)\n",
                sc_name, n, (unsigned)fs.jbd.start);
        local_failures++;
    }
    if ((fs.sb.feature_incompat & 0x4u) != 0) {
        fprintf(stderr, "  [%s/N=%d] recover: RECOVER still set on fs.sb (feature_incompat=0x%x)\n",
                sc_name, n, (unsigned)fs.sb.feature_incompat);
        local_failures++;
    }
    if (fs.jbd.replay_active != 0) {
        fprintf(stderr, "  [%s/N=%d] recover: replay_active=%u after recovery (expected 0)\n",
                sc_name, n, (unsigned)fs.jbd.replay_active);
        local_failures++;
    }

    /* Capture file content + size for downstream assertions. */
    target_ino = ext4_path_lookup(&fs, "/target.txt");
    if (target_ino != 0 && ext4_inode_read(&fs, target_ino, &inode) == 0) {
        out->file_size = inode.size;
        if (inode.size >= BLOCK_SIZE) {
            if (ext4_file_read_block(&fs, &inode, /*logical=*/0, out->block) == 0) {
                out->have_block = 1;
            }
        }
    }

    file_bdev_close(bd);

    if (e2fsck) {
        int erc = run_e2fsck(e2fsck, img);
        if (erc != 0) {
            fprintf(stderr, "  [%s/N=%d] recover: e2fsck -fn returned %d (expected 0)\n",
                    sc_name, n, erc);
            local_failures++;
        }
    }
    return local_failures;
}

/* Test one fault point in one scenario.
 *   pre_action: optional clean (un-faulted) write run before the faulted
 *               action, to put the journal in a non-initial state.
 *   action:     the faulted write under test.
 *   pre_byte/post_byte: expected pre- and post-action content for the
 *               first block of /target.txt (used for torn-write check).
 *               If post_byte == 0, content check is skipped (extend
 *               case — first block is unchanged either way).
 *   expect_extended: 1 if the action grows the file from 1 to 2 blocks. */
struct scenario {
    const char *name;
    int       (*pre_action)(struct blockdev *bd);
    int       (*action_under_fault)(struct blockdev *bd);
    int         max_fault_point;
    uint8_t     pre_byte;       /* expected byte fill of block 0 pre-action */
    uint8_t     post_byte;      /* expected byte fill of block 0 post-action; 0=skip */
    uint8_t     expect_extended;
    uint64_t    pre_size;
    uint64_t    post_size;
};

static int preamble_inplace_A_to_B(struct blockdev *bd) {
    return do_inplace_write(bd, 'B');
}
static int faulted_inplace_A_to_B(struct blockdev *bd) {
    return do_inplace_write(bd, 'B');
}
static int faulted_inplace_B_to_C(struct blockdev *bd) {
    return do_inplace_write(bd, 'C');
}
static int faulted_extend(struct blockdev *bd) {
    return do_extend_write(bd, 'X');
}

static int run_scenario(const struct scenario *sc, const char *clean,
                        const char *work, const char *e2fsck,
                        int *phase_hits_out) {
    int n;
    int sc_failures = 0;

    fprintf(stdout, "  scenario: %s (fault N=0..%d)\n", sc->name, sc->max_fault_point);
    for (n = 0; n <= sc->max_fault_point; n++) {
        struct blockdev        *bd;
        struct recovery_result  res;
        int                     prior_failures = total_failures;

        if (cp_file(clean, work) != 0) {
            fprintf(stderr, "  N=%d: cp failed\n", n);
            total_failures++;
            continue;
        }

        if (sc->pre_action) {
            bd = file_bdev_open_rw(work);
            if (!bd) { fprintf(stderr, "  N=%d: open(pre) failed\n", n); total_failures++; continue; }
            (void)sc->pre_action(bd);
            file_bdev_close(bd);
        }

        bd = file_bdev_open_rw(work);
        if (!bd) {
            fprintf(stderr, "  N=%d: open failed\n", n);
            total_failures++;
            continue;
        }
        file_bdev_set_fail_after(bd, n);
        (void)sc->action_under_fault(bd);
        file_bdev_close(bd);

        {
            int rc = recover_and_check(work, e2fsck, sc->name, n, &res);
            sc_failures += rc;
            total_failures += rc;
        }
        phase_hits_out[res.phase]++;
        phase_hits_global[res.phase]++;

        /* Content sanity: block 0 must be either pre_byte or post_byte. */
        if (sc->post_byte != 0 && res.have_block) {
            uint8_t b = res.block[0];
            uint32_t i;
            int homogeneous = 1;
            for (i = 0; i < BLOCK_SIZE; i++) {
                if (res.block[i] != b) { homogeneous = 0; break; }
            }
            if (!homogeneous) {
                fprintf(stderr, "  N=%d: torn block (not homogeneous)\n", n);
                total_failures++;
            } else if (b != sc->pre_byte && b != sc->post_byte) {
                fprintf(stderr, "  N=%d: block fill 0x%02x is neither pre 0x%02x nor post 0x%02x\n",
                        n, b, sc->pre_byte, sc->post_byte);
                total_failures++;
            }
        }

        /* Size sanity: must equal pre_size or post_size. */
        if (sc->post_size != sc->pre_size) {
            if (res.file_size != sc->pre_size && res.file_size != sc->post_size) {
                fprintf(stderr, "  N=%d: file_size=%llu is neither pre=%llu nor post=%llu\n",
                        n, (unsigned long long)res.file_size,
                        (unsigned long long)sc->pre_size,
                        (unsigned long long)sc->post_size);
                total_failures++;
            }
        }

        if (total_failures != prior_failures) {
            fprintf(stderr, "  N=%d: %d failure(s) [phase=%s]\n",
                    n, total_failures - prior_failures, phase_name(res.phase));
        }
    }
    return sc_failures;
}

int main(int argc, char **argv) {
    const char *clean = (argc > 1) ? argv[1] : "tests/images/write.img";
    const char *work  = (argc > 2) ? argv[2] : "tests/images/crash-recovery-test.img";
    const char *e2fsck;
    int         total_points = 0;
    int         i;

    /* Scenario table. pre_size/post_size are in bytes; for in-place
     * writes both are BLOCK_SIZE (file shape unchanged), for extend
     * pre=BLOCK_SIZE and post=2*BLOCK_SIZE. */
    struct scenario scenarios[] = {
        {
            .name               = "in-place A→B (single txn)",
            .pre_action         = NULL,
            .action_under_fault = faulted_inplace_A_to_B,
            .max_fault_point    = MAX_FAULT_POINT_INPLACE,
            .pre_byte           = 'A',
            .post_byte          = 'B',
            .pre_size           = BLOCK_SIZE,
            .post_size          = BLOCK_SIZE,
        },
        {
            .name               = "in-place B→C after clean A→B (multi-txn)",
            .pre_action         = preamble_inplace_A_to_B,
            .action_under_fault = faulted_inplace_B_to_C,
            .max_fault_point    = MAX_FAULT_POINT_INPLACE,
            .pre_byte           = 'B',
            .post_byte          = 'C',
            .pre_size           = BLOCK_SIZE,
            .post_size          = BLOCK_SIZE,
        },
        {
            .name               = "extend +1 block (5-trans-block txn)",
            .pre_action         = NULL,
            .action_under_fault = faulted_extend,
            .max_fault_point    = MAX_FAULT_POINT_EXTEND,
            .pre_byte           = 'A',
            .post_byte          = 0,        /* block 0 unchanged either way */
            .expect_extended    = 1,
            .pre_size           = BLOCK_SIZE,
            .post_size          = 2u * BLOCK_SIZE,
        },
    };

    e2fsck = find_e2fsck();
    if (!e2fsck) {
        fprintf(stderr, "host_crash_recovery_test: WARNING — e2fsck not found, skipping fsck checks\n");
    }

    for (i = 0; i < (int)(sizeof scenarios / sizeof scenarios[0]); i++) {
        int phase_hits[PHASE_COUNT] = {0};
        run_scenario(&scenarios[i], clean, work, e2fsck, phase_hits);
        fprintf(stdout, "    phase hits: pre-RECOVER=%d  orphan-RECOVER=%d  jsb-dirty=%d\n",
                phase_hits[PHASE_PRE_RECOVER],
                phase_hits[PHASE_ORPHAN_RECOVER],
                phase_hits[PHASE_JSB_DIRTY]);
        total_points += scenarios[i].max_fault_point + 1;
    }

    /* Cross-scenario coverage: every phase class must be exercised. */
    if (phase_hits_global[PHASE_PRE_RECOVER] == 0) {
        fprintf(stderr, "FAIL: no fault point landed in PRE_RECOVER\n");
        total_failures++;
    }
    if (phase_hits_global[PHASE_ORPHAN_RECOVER] == 0) {
        fprintf(stderr, "FAIL: no fault point landed in ORPHAN_RECOVER\n");
        total_failures++;
    }
    if (phase_hits_global[PHASE_JSB_DIRTY] == 0) {
        fprintf(stderr, "FAIL: no fault point landed in JSB_DIRTY (real journal-replay path not exercised)\n");
        total_failures++;
    }

    if (total_failures == 0) {
        printf("host_crash_recovery_test: all %d fault points across %d scenarios "
               "recovered cleanly%s; phase coverage: pre=%d orphan=%d jsb=%d\n",
               total_points, (int)(sizeof scenarios / sizeof scenarios[0]),
               e2fsck ? " (e2fsck -fn passed each)" : " (e2fsck skipped)",
               phase_hits_global[PHASE_PRE_RECOVER],
               phase_hits_global[PHASE_ORPHAN_RECOVER],
               phase_hits_global[PHASE_JSB_DIRTY]);
        return 0;
    }
    fprintf(stderr, "host_crash_recovery_test: %d FAILURE(S) across %d fault points\n",
            total_failures, total_points);
    return 1;
}
