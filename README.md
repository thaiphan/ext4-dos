# ext4-dos

So you've got a Linux disk with an ext4 partition, and you also run DOS — maybe FreeDOS on old hardware, maybe MS-DOS 4 for the nostalgia, maybe both in DOSBox-X. Wouldn't it be nice if DOS could just... see those Linux files? Like a normal drive letter?

That's what this is. Drop one `.EXE` into your `CONFIG.SYS`, reboot, and your ext4 partition shows up as `D:`. `DIR`, `COPY`, `TYPE`, file managers, whatever — they all just work, the same way they work on a FAT drive.

**Reads and writes both work.** You can copy files off, copy files on, make directories, delete stuff, rename. It goes through the ext4 journal properly so your Linux box won't complain about a dirty filesystem afterwards.

Tested end-to-end on **FreeDOS 1.4** and **MS-DOS 4.0**.

---

## Download

Grab the `.EXE` files from the latest [GitHub release](../../releases). You probably only need one:

| File       | What it does                                               |
| ---------- | ---------------------------------------------------------- |
| `ext4.exe` | The TSR that mounts your ext4 partition as a drive letter. |

Everything else in the release is diagnostic/test tooling — see [the full binary list](#all-the-binaries) if you're curious.

---

## Installation

One line in `CONFIG.SYS` and you're done:

```text
INSTALL=C:\EXT4.EXE -q
```

Copy `EXT4.EXE` to `C:\`, add that line, reboot. The TSR scans your BIOS hard disks for an ext4 partition, grabs the first free drive letter (usually `D:`), and mounts there. You'll see a short banner on boot confirming what it found.

From then on, it works like any other drive:

```text
C:\> DIR D:
 Volume in drive D has no label
 Directory of D:\

HELLO    TXT        42  05-04-2026  12:00a
PROJECTS     <DIR>       05-01-2026   9:15a
README   MD       1234  04-28-2026   3:44p
         3 file(s)          1276 bytes
     57,344,000 bytes free

C:\> TYPE D:\HELLO.TXT
Hello from Linux!

C:\> COPY D:\PROJECTS\*.C C:\SRC\
...

C:\> COPY C:\MYPROG.EXE D:\
...

C:\> MD D:\BACKUP
C:\> REN D:\OLD.TXT NEW.TXT
C:\> DEL D:\JUNK.TMP
```

To unload the TSR and free the memory:

```text
C:\> EXT4 -U
ext4-dos uninstalled
```

### Multiple disks or a specific drive letter

By default it scans and picks automatically. You can override:

```text
C:\> EXT4 0x82           Use the third BIOS hard disk (auto-pick letter).
C:\> EXT4 Z:             Auto-scan, but mount at Z: specifically.
C:\> EXT4 0x81 Z:        Pin both: disk 0x81, mounted at Z:.
```

Numbers can be decimal or `0x`-prefixed. `LASTDRIVE=` in `CONFIG.SYS` needs to cover the letter you want.

---

## What works

### FreeDOS 1.4

| Operation              | Works?                                         |
| ---------------------- | ---------------------------------------------- |
| `DIR D:`               | ✓ Real timestamps, real sizes, real free space |
| `DIR D:\*.TXT`         | ✓ Wildcard filtering works                     |
| `TYPE D:\file.txt`     | ✓                                              |
| `COPY D:\... C:\...`   | ✓ Copy files off                               |
| `COPY C:\... D:\...`   | ✓ Copy files onto the ext4 partition           |
| `MD` / `RD`            | ✓ Create and remove directories                |
| `DEL`                  | ✓                                              |
| `REN` (same directory) | ✓                                              |
| Cross-directory move   | ✓ via `ext4mv.exe` (see below)                 |
| Long filenames         | via 8.3 aliases (see below)                    |
| Files > 4 GB           | not yet                                        |

### MS-DOS 4.0

Same as FreeDOS with two cosmetic quirks:

- **`DIR` free space display is wrong for large partitions.** If your ext4 partition is bigger than ~96 MB, the `bytes free` line in `DIR` will show a garbage number. This is a bug inside MS-DOS 4's `COMMAND.COM` — the actual free space is tracked correctly, reads and writes work fine, it's purely a display issue. FreeDOS shows the right number.
- **Wildcard `DIR D:\*.TXT` shows nothing** under MS-DOS 4, even though the files are there. Plain `DIR D:` works. This is also inside `COMMAND.COM`. Workaround: use a third-party dir lister, or switch to FreeDOS for scripting.

---

## Long filenames

ext4 supports long filenames. DOS doesn't (well, not really). So files with names that don't fit in 8.3 format get exposed with a deterministic short alias: `VERY~876.TXT`, where `876` is a short hash of the full filename.

The alias is stable across reboots and you can use it normally:

```text
C:\> TYPE D:\VERY~876.TXT      opens verylongname1.txt on the Linux side
```

You can see all the aliases with `DIR D:`. They're not pretty but they work.

---

## All the binaries

The release also includes some diagnostic tools:

| File          | What it does                                                      |
| ------------- | ----------------------------------------------------------------- |
| `ext4chk.exe` | Check whether the TSR is currently loaded.                        |
| `ext4cli.exe` | Print ext4 superblock info from DOS (like `dumpe2fs` but in DOS). |
| `ext4dir.exe` | Raw directory listing via INT 21h (useful for debugging).         |
| `ext4cnt.exe` | Show per-operation call counters from the loaded TSR.             |
| `ext4dmp.exe` | Dump internal diagnostic state from the loaded TSR.               |
| `ext4xfr.exe` | Test the extended free-space API.                                 |
| `ext4tr.exe`  | Truncate a file (used by the test suite).                         |
| `ext4mv.exe`  | Cross-directory rename (COMMAND.COM's `REN` is same-dir only).    |

---

## Why does this exist?

The existing DOS ext tools are ext2-era. Modern Linux disks use ext4-specific stuff — extent trees, htree directories, a journal, metadata checksums — that those older tools either don't understand or silently misread. This project tries to do it properly, or bail out loudly if it hits something it doesn't support.

- Real-mode 16-bit, no DOS extender needed, works on a stock FreeDOS or MS-DOS 4 install.
- Refuses to mount if an unsupported ext4 feature flag is present (no silent misreads).
- All writes go through the journal with metadata checksums, so your Linux box sees a clean filesystem.

---

## Docs

- [docs/dos-internals.md](docs/dos-internals.md) — deep dive into how DOS redirectors work, the INT 2Fh subfunction map, DOS version quirks, and all the things we had to figure out the hard way. Good reading if you're building something similar.
- [docs/building.md](docs/building.md) — building from source, running the test suite, and how to use the DOSBox-X heavy debugger.
- [docs/references.md](docs/references.md) — the reference implementations we cross-checked against.

---

## License

MIT. See [LICENSE](LICENSE).
