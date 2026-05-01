# ext4-dos

A 16-bit DOS TSR that exposes an ext4 partition as a drive letter, so DOS programs (`DIR`, `TYPE`, file managers, etc.) can read it the same way they read a FAT drive.

Targets FreeDOS 1.4 and MS-DOS 4.0. Both verified end-to-end: `DIR Y:` lists ext4 entries with real timestamps and free space, `TYPE Y:\HELLO.TXT` reads file content. Read-only for v1; write support is explicitly a separate later project.

## Download

Pre-built `.EXE` binaries are attached to each [GitHub release](../../releases). Each is a single-file download:

| Binary        | Purpose |
|---            |---|
| `ext4.exe`    | The redirector TSR. Load via `CONFIG.SYS INSTALL=` or from `AUTOEXEC.BAT`. |
| `ext4cli.exe` | DOS-side ext4 inspector (analogous to `dumpe2fs`). |
| `ext4chk.exe` | Probe whether the TSR is loaded. |
| `ext4dir.exe` | Raw INT 21h FindFirst smoke test against the TSR. |
| `ext4cnt.exe` | Read per-subfunction call counters from a loaded TSR. |
| `ext4dmp.exe` | Dump diagnostic capture state from a loaded TSR. |

The `ext4` prefix marks them as belonging to the ext4 module; future modules (LFN, networking, etc.) will follow the same pattern.

## Known limitations

- **Read-only.** Writes are out of scope for v1.
- **MS-DOS 4: DIR's "bytes free" display is wrong for ext4 disks larger than ~96 MB.** The redirector returns the right numbers; only MS-DOS 4 DIR's printed `bytes free` line is wrong (a buggy display path in MS-DOS 4 itself). FreeDOS shows the correct value. *Actual file operations are unaffected.* Future fix: implement `INT 2Fh AX=11A3h` (Get Extended Free Space) so callers that opt in get full 32-bit precision.
- **Long filenames** are not exposed — names get truncated to 8.3. The DOS LFN ecosystem (DOSLFN.COM etc.) is FAT-only and has no protocol for asking redirectors about long names. See [`docs/dos-internals.md`](docs/dos-internals.md) for the full story.

## DOS internals: working notes

A lot of what made this project work is folklore — undocumented DOS quirks, redirector subfunction dispatch, CDS flag bits that differ between FreeDOS and MS-DOS 4, etc. Everything we learned is captured in [`docs/dos-internals.md`](docs/dos-internals.md). Required reading if you're picking up the project or writing a different DOS redirector.

Builds for every push are also available from the [Actions](../../actions) tab under the `dosix-binaries` artifact (auth required).

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
