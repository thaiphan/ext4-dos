# MS-DOS 4 test fixture

`msdos4-source.img` is the bootable 1.44 MB floppy image from Microsoft's
April 2024 open-source release of MS-DOS 4.0
(<https://github.com/microsoft/MS-DOS>, MIT license, redistributed here
unchanged).

`scripts/run-msdos4-test.sh` (target: `make msdos4-test`) makes a fresh
copy each run, injects our TSR + test binaries + a CONFIG.SYS that loads
the redirector via INSTALL=, then boots it in DOSBox-X with the ext4
fixture attached at INT 13h drive 0x81.

## Status: partial

- TSR install works — CONFIG.SYS `INSTALL=A:\TSR.EXE` runs successfully,
  the diagnostic banner confirms LOL/CDS hookup and ext4 mount, and Y:
  is marked as a redirector drive.
- COMMAND.COM load fails afterward with "Bad or missing Command
  Interpreter" — root cause not isolated. Same failure across multiple
  resident sizes and CONFIG.SYS shapes.
- Full `DIR Y:` / `TYPE Y:\HELLO.TXT` testing under MS-DOS 4 is blocked
  by the COMMAND.COM failure, not by anything in our redirector. The
  same code paths work end-to-end under FreeDOS.

The header comment in `scripts/run-msdos4-test.sh` documents every
rabbit hole we've already explored — read it before retrying or you
will repeat them. References we cloned to investigate live in
`references/msdos4/` (Microsoft's MIT-licensed v4.0 source).

The working copy `test.img` and any logs live alongside this README and
are gitignored — only the pristine `msdos4-source.img` is committed.

To override the source location (e.g. point at a different MS-DOS
version):

```sh
MSDOS4_SRC=/path/to/other.img make msdos4-test
```
