# References

We don't vendor any third-party code. lwext4 and GRUB live as gitignored clones / files under `references/`; we read them, write our own implementation, and cross-check behavior. Where a non-obvious detail in our code came from a specific reference, it's noted in a comment.

## lwext4 — `references/lwext4/`

- Source: https://github.com/gkostka/lwext4
- License: BSD (no contagion)
- Strongest for:
  - Clean filesystem-vs-block-device abstraction (our boundary follows the same shape).
  - Both read and write paths — the primary reference once write support starts (post-v1).
  - Journal (jbd2) record decoding and replay.
  - Separation between on-disk types (`include/ext4_types.h`) and operations (`include/ext4_super.h`, `src/ext4_super.c`, etc.).

## GRUB ext2/3/4 reader — `references/grub-ext2.c`

- Source: `grub-core/fs/ext2.c` from GNU GRUB (single self-contained file).
  Fetched from https://git.savannah.gnu.org/cgit/grub.git/plain/grub-core/fs/ext2.c
- License: **GPLv3 — do not copy code, only read for understanding.**
- Strongest for:
  - Real-world read-path correctness — every Linux boot exercises this code, so it has been beaten on by every weird ext4 configuration in the wild.
  - Compact and focused (~1100 lines for the entire reader, read-only by design).
  - Feature-flag handling pattern (see `EXT2_DRIVER_SUPPORTED_INCOMPAT` and `EXT2_DRIVER_IGNORED_INCOMPAT`) — direct precedent for our refusal policy.

## ext4 on-disk format spec

- https://www.kernel.org/doc/html/latest/filesystems/ext4/
- Authoritative source for byte offsets, field widths, and feature flag semantics. Cite in code comments when making a non-obvious format-interpretation decision.
