# ext4-dos

A 16-bit DOS TSR that exposes an ext4 partition as a drive letter, so DOS programs (`DIR`, `TYPE`, file managers, etc.) can read it the same way they read a FAT drive.

Targets FreeDOS 1.4 primarily; MS-DOS 4 as a compatibility target.

## Status

Pre-alpha. Nothing works yet.

## Why

Existing DOS-side ext support is ext2-era. Modern Linux disks use ext4-specific features — extent trees, htree directories, the journal, metadata checksums — that older tools don't understand and silently misread. This project aims to do it right, or refuse cleanly when it can't.

## Scope (v1)

- Read-only.
- Real-mode 16-bit only — no DOS extender required, runs on a stock FreeDOS install.
- Refuses to mount when an ext4 feature flag we don't explicitly support is present, rather than risk silent misreads.

Write support is a separate, later project.

## License

MIT. See [LICENSE](LICENSE).

## References

This project does not vendor or link any third-party code. We study these implementations as cross-checked references and write our own:

- [lwext4](https://github.com/gkostka/lwext4) — BSD-licensed embedded ext2/3/4 reader and writer; clean abstraction, journal handling.
- GRUB `grub-core/fs/ext2.c` — GPLv3, used by every Linux boot, strong real-world correctness on the read path.
- [ext4 on-disk format spec](https://www.kernel.org/doc/html/latest/filesystems/ext4/) — primary reference for the format itself.
