#include "features.h"
#include "superblock.h"
#include <stdio.h>
#include <string.h>

static const struct {
    uint32_t bit;
    const char *name;
} INCOMPAT_NAMES[] = {
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

int ext4_features_check_supported(const struct ext4_superblock *sb,
                                  char *err, size_t err_len) {
    uint32_t unsupported = sb->feature_incompat & ~(uint32_t)EXT4_V1_INCOMPAT_SUPPORTED;
    if (!unsupported) {
        if (err && err_len) err[0] = '\0';
        return 0;
    }
    if (err && err_len) {
        size_t n = sizeof INCOMPAT_NAMES / sizeof INCOMPAT_NAMES[0];
        const char *name = NULL;
        uint32_t bit = 0;
        size_t i;
        for (i = 0; i < n; i++) {
            if (unsupported & INCOMPAT_NAMES[i].bit) {
                bit = INCOMPAT_NAMES[i].bit;
                name = INCOMPAT_NAMES[i].name;
                break;
            }
        }
        if (name) {
            snprintf(err, err_len,
                     "incompat feature '%s' (0x%x) not supported in v1",
                     name, (unsigned)bit);
        } else {
            snprintf(err, err_len,
                     "incompat features 0x%x not supported in v1",
                     (unsigned)unsupported);
        }
    }
    return -1;
}
