#include "features.h"
#include "superblock.h"
#include <stdio.h>
#include <string.h>

struct feat_name { uint32_t bit; const char *name; };

static const struct feat_name INCOMPAT_NAMES[] = {
    { 0x00000001u, "compression" },
    { 0x00000002u, "filetype" },
    { 0x00000004u, "recover" },
    { 0x00000008u, "journal_dev" },
    { 0x00000010u, "meta_bg" },
    { 0x00000040u, "extents" },
    { 0x00000080u, "64bit" },
    { 0x00000100u, "mmp" },
    { 0x00000200u, "flex_bg" },
    { 0x00000400u, "ea_inode" },
    { 0x00001000u, "dirdata" },
    { 0x00002000u, "csum_seed" },
    { 0x00004000u, "largedir" },
    { 0x00008000u, "inline_data" },
    { 0x00010000u, "encrypt" },
    { 0x00020000u, "casefold" },
    { 0x00040000u, "verity" },
    { 0x00080000u, "orphan_file" },
};

static const struct feat_name RO_COMPAT_NAMES[] = {
    { 0x00000001u, "sparse_super" },
    { 0x00000002u, "large_file" },
    { 0x00000004u, "btree_dir" },
    { 0x00000008u, "huge_file" },
    { 0x00000010u, "gdt_csum" },
    { 0x00000020u, "dir_nlink" },
    { 0x00000040u, "extra_isize" },
    { 0x00000100u, "quota" },
    { 0x00000200u, "bigalloc" },
    { 0x00000400u, "metadata_csum" },
    { 0x00000800u, "replica" },
    { 0x00001000u, "readonly" },
    { 0x00002000u, "project" },
    { 0x00008000u, "verity" },
    { 0x00020000u, "orphan_present" },
};

static int report_unsupported(const char *kind, uint32_t bits,
                              const struct feat_name *table,
                              size_t table_len, char *err, size_t err_len) {
    size_t i;
    for (i = 0; i < table_len; i++) {
        if (bits & table[i].bit) {
            snprintf(err, err_len,
                     "%s feature '%s' (0x%x) not supported in v1",
                     kind, table[i].name, (unsigned)table[i].bit);
            return -1;
        }
    }
    snprintf(err, err_len,
             "%s features 0x%x not supported in v1", kind, (unsigned)bits);
    return -1;
}

int ext4_features_check_supported(const struct ext4_superblock *sb,
                                  char *err, size_t err_len) {
    uint32_t bad_incompat  = sb->feature_incompat  & ~(uint32_t)EXT4_V1_INCOMPAT_SUPPORTED;
    uint32_t bad_ro_compat = sb->feature_ro_compat & ~(uint32_t)EXT4_V1_RO_COMPAT_SUPPORTED;

    if (!bad_incompat && !bad_ro_compat) {
        if (err && err_len) err[0] = '\0';
        return 0;
    }
    if (err && err_len) {
        if (bad_incompat) {
            return report_unsupported(
                "incompat", bad_incompat, INCOMPAT_NAMES,
                sizeof INCOMPAT_NAMES / sizeof INCOMPAT_NAMES[0],
                err, err_len);
        }
        return report_unsupported(
            "ro_compat", bad_ro_compat, RO_COMPAT_NAMES,
            sizeof RO_COMPAT_NAMES / sizeof RO_COMPAT_NAMES[0],
            err, err_len);
    }
    return -1;
}
