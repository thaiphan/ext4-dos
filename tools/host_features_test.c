/* Host-side test for ext4_features_check_supported.
 *
 * Builds a minimum viable superblock struct in memory (no disk required)
 * and asserts that the check function refuses unsupported feature bits
 * cleanly with the right error message — and accepts the known-good set.
 *
 * Run as part of `make host-test`. Failure exits non-zero so CI catches
 * regressions. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "ext4/superblock.h"
#include "ext4/features.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

static void clean_sb(struct ext4_superblock *sb) {
    memset(sb, 0, sizeof *sb);
    sb->magic = EXT4_SUPERBLOCK_MAGIC;
    sb->block_size = 4096;
    sb->inode_size = 256;
    sb->feature_incompat = EXT4_V1_INCOMPAT_SUPPORTED;
    sb->feature_ro_compat = EXT4_V1_RO_COMPAT_SUPPORTED;
}

static void test_clean_sb(void) {
    struct ext4_superblock sb;
    char err[128];
    int rc;

    clean_sb(&sb);
    rc = ext4_features_check_supported(&sb, err, sizeof err);
    CHECK(rc == 0, "expected mount to succeed for full-supported sb, got rc=%d (err='%s')",
          rc, err);
    CHECK(err[0] == '\0', "expected empty err on success, got '%s'", err);
}

static void test_unsupported_incompat(uint32_t bit, const char *name) {
    struct ext4_superblock sb;
    char err[128];
    int rc;

    clean_sb(&sb);
    sb.feature_incompat |= bit;
    rc = ext4_features_check_supported(&sb, err, sizeof err);
    CHECK(rc != 0, "expected refusal for incompat=0x%x (%s), got success", bit, name);
    CHECK(strstr(err, name) != NULL,
          "expected err to mention '%s' for bit 0x%x, got '%s'", name, bit, err);
    CHECK(strstr(err, "incompat") != NULL,
          "expected err to say 'incompat', got '%s'", err);
}

static void test_unsupported_ro_compat(uint32_t bit, const char *name) {
    struct ext4_superblock sb;
    char err[128];
    int rc;

    clean_sb(&sb);
    sb.feature_ro_compat |= bit;
    rc = ext4_features_check_supported(&sb, err, sizeof err);
    CHECK(rc != 0, "expected refusal for ro_compat=0x%x (%s), got success", bit, name);
    CHECK(strstr(err, name) != NULL,
          "expected err to mention '%s' for bit 0x%x, got '%s'", name, bit, err);
    CHECK(strstr(err, "ro_compat") != NULL,
          "expected err to say 'ro_compat', got '%s'", err);
}

int main(void) {
    /* Known-good baseline must pass. */
    test_clean_sb();

    /* Each must-refuse incompat bit. */
    test_unsupported_incompat(0x00000001u, "compression");
    test_unsupported_incompat(0x00000008u, "journal_dev");
    test_unsupported_incompat(0x00000100u, "mmp");
    test_unsupported_incompat(0x00000400u, "ea_inode");
    test_unsupported_incompat(0x00008000u, "inline_data");
    test_unsupported_incompat(0x00010000u, "encrypt");
    test_unsupported_incompat(0x00020000u, "casefold");

    /* Each must-refuse ro_compat bit (BIGALLOC is the killer for our
     * extent math — anything else is "RO is fine to read around"). */
    test_unsupported_ro_compat(0x00000200u, "bigalloc");

    if (failures == 0) {
        printf("host_features_test: all asserts passed\n");
        return 0;
    }
    fprintf(stderr, "host_features_test: %d FAILURE(S)\n", failures);
    return 1;
}
