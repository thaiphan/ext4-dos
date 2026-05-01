#ifndef EXT4_DIR_H
#define EXT4_DIR_H

#include <stdint.h>
#include <stddef.h>

#define EXT4_FT_REGULAR 1
#define EXT4_FT_DIR     2
#define EXT4_FT_SYMLINK 7

struct ext4_fs;
struct ext4_inode;

struct ext4_dir_entry {
    uint32_t inode;
    uint8_t  file_type;
    uint8_t  name_len;
    char     name[256];   /* null-terminated copy */
};

typedef int (*ext4_dir_cb)(const struct ext4_dir_entry *e, void *userdata);

/* Walk every entry in a directory inode, calling `cb` once per entry.
 * If cb returns non-zero, iteration stops early and that value is returned. */
int ext4_dir_iter(struct ext4_fs *fs, const struct ext4_inode *dir,
                  ext4_dir_cb cb, void *userdata);

/* Find a single name in a directory. Returns inode number, or 0 if not found. */
uint32_t ext4_dir_lookup(struct ext4_fs *fs, const struct ext4_inode *dir,
                         const char *name);

/* Resolve an absolute path (starting with '/') to an inode number, walking
 * from root (inode 2). Returns 0 if any component is missing or not a dir.
 * Does not follow symlinks. "." and ".." resolve via their on-disk entries. */
uint32_t ext4_path_lookup(struct ext4_fs *fs, const char *path);

#endif
