# ext4-dos

A 16-bit DOS TSR that exposes an ext4 partition as a drive letter, so DOS programs (`DIR`, `TYPE`, `COPY`, file managers, etc.) can use it the same way they use a FAT drive.

Targets FreeDOS 1.4 and MS-DOS 4.0. Both verified end-to-end: `DIR D:` lists ext4 entries with real timestamps and free space, `TYPE D:\HELLO.TXT` reads files, `COPY`/`REN`/`DEL`/`MD`/`RD` write through to the underlying ext4 filesystem (with journal commits + metadata checksums + post-run `e2fsck` clean as the regression net).

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
| `ext4xfr.exe` | Verify `INT 2Fh AX=11A3h` (Get Extended Free Space) against the loaded TSR, both directly and via `INT 21h AX=7303h`. |
| `ext4tr.exe`  | Truncate a file via `INT 21h AH=40h CX=0` ("set EOF to current SFT pos"). Used by the test suite to exercise truncate-down. |
| `ext4mv.exe`  | Cross-directory rename via `INT 21h AH=56h` (`COMMAND.COM`'s `REN` only does same-dir). Used by the test suite to exercise `REM_RENAME`'s cross-parent path. |

The `ext4` prefix marks them as belonging to the ext4 module; future modules (LFN, networking, etc.) will follow the same pattern.

## Usage

The expected zero-config setup. Add one line to `CONFIG.SYS`:

```text
LASTDRIVE=Z
INSTALL=C:\EXT4.EXE -q
```

Reboot. Every boot, the TSR scans BIOS hard disks 0x80..0x83 for an ext4 partition, picks the first free drive letter (typically D:), mounts there, and prints a banner showing what it found. From then on:

```text
C:\> DIR D:              ext4 root listing — real timestamps, real sizes.
C:\> TYPE D:\README.MD   Read a file.
C:\> COPY D:\DATA\*.TXT C:\BACKUP\    Copy files off. Wildcards and subdirs work.
C:\> COPY MYFILE.TXT D:\ Write a file (creates, extends, journals).
C:\> MD D:\NEW           Make a directory.
C:\> DEL D:\OLD.TXT      Delete a file.
C:\> REN D:\A.TXT B.TXT  Rename within a directory.
C:\> CD D:\PROJECTS      Change into ext4 directories like normal.

C:\> EXT4 -U             Uninstall. Frees TSR memory; the drive disappears.
ext4-dos uninstalled
```

This mirrors how MSCDEX exposes a CD-ROM. The auto-pick chooses the lowest free slot at or above D:, so the letter is stable as long as your other drivers are.

Writes go through an ext4 journal commit per operation (metadata checksums included). The regression suite runs `e2fsck -fn` on the partition after every test boot — failures fail the build.

Long filenames on disk are exposed as 8.3 short names with deterministic `~HHH` aliases (where `HHH` is a hex hash). `DIR` shows you the alias; `TYPE D:\VERY~876.TXT` opens the underlying long-named file. The mapping is stable across runs.

### Manual overrides

If auto-detection picks the wrong disk (multiple ext4 partitions) or you want a specific letter:

```text
C:\> EXT4 0x82           Mount the third BIOS hard disk (auto-pick letter).
C:\> EXT4 Z:             Auto-scan for ext4, mount at Z: (override letter).
C:\> EXT4 0x81 Z:        Pin both: drive 0x81 mounted at Z:.
```

Drive letters require the trailing `:` (so `Z` alone is treated as nothing — only `Z:` is parsed). Numbers can be decimal or `0x`-prefixed. `LASTDRIVE=` in `CONFIG.SYS` must be high enough to cover the letter you want.

## Known limitations

- **MS-DOS 4: DIR's "bytes free" display is wrong for ext4 disks larger than ~96 MB.** The redirector returns the right numbers; only MS-DOS 4 DIR's printed `bytes free` line is wrong (a buggy display path inside MS-DOS 4's `COMMAND.COM` itself, not the redirector). FreeDOS displays the correct value. *Actual file operations are unaffected — file sizes, free space used by writes, etc. all work correctly; only that one DIR line is misformatted.* The redirector implements `INT 2Fh AX=11A3h` (Get Extended Free Space) for callers that opt in via `INT 21h AX=7303h`, but MS-DOS 4 `DIR` predates that API and only ever issues the legacy 16-bit query, so a redirector-side fix can't reach it. Workaround: use a different shell (FreeDOS's `FREECOM`) or a third-party DIR utility.
- **Long filenames** are exposed only as 8.3 aliases (`VERY~876.TXT`), not as their real long names. The DOS LFN ecosystem (DOSLFN.COM etc.) is FAT-only and has no protocol for asking redirectors about long names. See [`docs/dos-internals.md`](docs/dos-internals.md) for the full story.
- **Renaming a directory across parents** isn't supported (`ext4_rename_xdir` refuses dirs — would also need to update the moved dir's `..` entry and adjust both parents' link counts). Cross-directory rename of regular files works via `INT 21h AH=56h` (use a tool that issues it directly; `COMMAND.COM`'s built-in `REN` is same-dir only).

## DOS internals: working notes

A lot of what made this project work is folklore — undocumented DOS quirks, redirector subfunction dispatch, CDS flag bits that differ between FreeDOS and MS-DOS 4, etc. Everything we learned is captured in [`docs/dos-internals.md`](docs/dos-internals.md). Required reading if you're picking up the project or writing a different DOS redirector.

Builds for every push are also available from the [Actions](../../actions) tab under the `dosix-binaries` artifact (auth required).

### Debugging the TSR's interaction with the DOS kernel

Several of the trickier bugs we hit needed a real CPU debugger — interactive `BPM` watchpoints and single-step into MS-DOS 4 kernel code. To reproduce that workflow:

```sh
bash scripts/setup-debugger.sh    # one-off, ~10 min: clones DOSBox-X, builds with --enable-heavy-debug
bash scripts/run-msdos4-debug.sh  # boots MS-DOS 4 in the heavy debugger
```

The setup script pins to a specific upstream DOSBox-X commit so the build is reproducible. Skip if you'll only ever run the smoke tests.

## Why

Existing DOS-side ext support is ext2-era. Modern Linux disks use ext4-specific features — extent trees, htree directories, the journal, metadata checksums — that older tools don't understand and silently misread. This project aims to do it right, or refuse cleanly when it can't.

## Scope

- Real-mode 16-bit only — no DOS extender required, runs on a stock FreeDOS install.
- Refuses to mount when an ext4 feature flag we don't explicitly support is present, rather than risk silent misreads.
- Reads + writes through an ext4 journal, with `metadata_csum` honored on both read and write. Post-run `e2fsck -fn` is the regression net — every test boot validates that the on-disk image is still clean.

## License

MIT. See [LICENSE](LICENSE).

## References

This project does not vendor or link any third-party code. We study these implementations as cross-checked references and write our own:

- [lwext4](https://github.com/gkostka/lwext4) — BSD-licensed embedded ext2/3/4 reader and writer; clean abstraction, journal handling.
- GRUB `grub-core/fs/ext2.c` — GPLv3, used by every Linux boot, strong real-world correctness on the read path.
- [ext4 on-disk format spec](https://www.kernel.org/doc/html/latest/filesystems/ext4/) — primary reference for the format itself.
