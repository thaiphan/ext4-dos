# MS-DOS 4 test fixture

`msdos4-source.img` is the bootable 1.44 MB floppy image from Microsoft's
April 2024 open-source release of MS-DOS 4.0
(<https://github.com/microsoft/MS-DOS>, MIT license, redistributed here
unchanged).

`scripts/run-msdos4-test.sh` (target: `make msdos4-test`) makes a fresh
copy each run, injects our TSR + test binaries + a CONFIG.SYS that loads
the redirector via INSTALL=, then boots it in DOSBox-X with the ext4
fixture attached at INT 13h drive 0x81.

## Status

- **Boot + TSR install: working.** CONFIG.SYS `INSTALL=A:\TSR.EXE` runs
  cleanly, COMMAND.COM loads, AUTOEXEC.BAT runs to completion, A> prompt
  reachable.
- **`DIR Y:`: working.** Lists ext4 entries with timestamps and free
  space. MS-DOS 4 dispatches FindFirst through the IFS-style
  `IFS_SEQ_SEARCH_FIRST` (INT 2Fh AL=0x19) variant, which we now handle.
- **`TYPE Y:\file`: not yet working.** MS-DOS 4 routes file opens through
  AL=0x2E (Extended Open) and our handler — currently aliased to AL=0x16
  — reports failure. Smaller follow-up: figure out the right SDA/SFT
  contract for ExtOpen.

The header comment in `scripts/run-msdos4-test.sh` documents the
investigation path so anyone re-visiting can skip the dead ends. The
MS-DOS 4 source lives at `references/msdos4/` (Microsoft v4.0, MIT).

The working copy `test.img` and any logs live alongside this README and
are gitignored — only the pristine `msdos4-source.img` is committed.

To override the source location (e.g. point at a different MS-DOS
version):

```sh
MSDOS4_SRC=/path/to/other.img make msdos4-test
```
