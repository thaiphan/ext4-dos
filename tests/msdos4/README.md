# MS-DOS 4 test fixture

`msdos4-source.img` is the bootable 1.44 MB floppy image from Microsoft's
April 2024 open-source release of MS-DOS 4.0
(<https://github.com/microsoft/MS-DOS>, MIT license, redistributed here
unchanged).

`scripts/run-msdos4-test.sh` (target: `make msdos4-test`) makes a fresh
copy each run, injects our TSR + test binaries + a CONFIG.SYS that loads
IFSFUNC and the redirector at boot, then boots it in DOSBox-X with the
ext4 fixture attached at INT 13h drive 0x81.

The working copy `test.img` and any logs live alongside this README and
are gitignored — only the pristine `msdos4-source.img` is committed.

To override the source location (e.g. point at a different MS-DOS
version):

```sh
MSDOS4_SRC=/path/to/other.img make msdos4-test
```
