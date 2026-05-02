# DOS internals: a working notes file

Everything in this document is the kind of thing that took *hours* to figure out, isn't obvious from reading the code, and isn't well-documented elsewhere on today's internet. If you're picking up this project — or writing a different DOS redirector entirely — this is the cheat sheet.

Structured roughly: foundational concepts → INT 2Fh AH=11h subfunction map → toolchain gotchas → DOS-version-specific quirks → known limitations.

---

## 1. How a DOS filesystem redirector actually works

A redirector is a TSR (`.EXE` that stays resident via `INT 21h AH=31h`) that exposes a synthetic drive letter to DOS by hooking `INT 2Fh AH=11h`. There's nothing magical — DOS dispatches every file operation on a "remote" drive through that interrupt, and your TSR responds by reading whatever it actually wants to expose (an ext4 partition, a network share, a tarball, …).

### The flow on every file op

```
user types "TYPE Y:\foo.txt"
  ↓
COMMAND.COM calls INT 21h AH=3Dh (Open)
  ↓
DOS kernel checks Y:'s CDS entry — flag bits say "redirected"
  ↓
DOS calls INT 2Fh AH=11h, AL=<some Open subfunction>, ES:DI = SFT
  ↓
your TSR's INT 2Fh hook sees AH=11h, dispatches by AL
  ↓
you read the file from your backing store, fill the SFT, return CF=0
  ↓
DOS returns a handle to COMMAND.COM
  ↓
COMMAND.COM calls INT 21h AH=3Fh (Read), kernel routes through INT 2Fh AL=08h
  ↓
your TSR Reads, returns bytes in user's buffer (at SDA+0x0C, NOT DS:DX)
  ↓
COMMAND.COM prints them
```

### Three structures DOS hands you that you must understand

**LOL (List of Lists)** — a kernel-internal struct returned by `INT 21h AH=52h`. Holds pointers to most of the things below. Layout differs slightly per DOS version; the offsets we care about (CDS array pointer at `LOL+0x16`, LASTDRIVE byte at `LOL+0x21`) are stable from DOS 3.1 onward.

**CDS (Current Directory Structure)** — array of 88-byte entries (one per drive letter, count from `LASTDRIVE=`). For your redirector to be reachable you must *write* the right flag bits (and a placeholder path) into the slot for your drive letter. DOS won't dispatch to your INT 2Fh hook for a drive whose CDS doesn't say "redirected."

CDS layout (from MS-DOS 4 `src/INC/CURDIR.INC`, also matches FreeDOS):

| Offset | Size  | Field |
|--------|-------|---|
| 0x00   | 67 B  | `curdir_text` — current path string (e.g. `"Y:\\\0"` for a fresh redirector) |
| 0x43   | 2 B   | `curdir_flags` — flag bits (see below) |
| 0x45   | 4 B   | `curdir_devptr` — local DPB pointer or net device pointer |
| 0x49   | 2 B   | `curdir_ID` — cluster of curdir / net ID |
| 0x4B   | 2 B   | reserved |
| 0x4D   | 2 B   | `curdir_user_word` |
| 0x4F   | 2 B   | `curdir_end` — offset where path resolution stops appending |
| 0x51   | 1 B   | `curdir_type` — DOS-4-only: 2 = IFS drive, 4 = netuse |
| 0x52   | 4 B   | `curdir_ifs_hdr` — DOS-4-only: ptr to IFS file system header |
| 0x56   | 2 B   | `curdir_fsda` |

CDS flag bits (`curdir_flags`):

| Bit    | Name | Meaning |
|--------|---|---|
| 0x8000 | `curdir_isnet` / `curdir_isifs` | network / IFS drive — your redirector handles it |
| 0x4000 | `curdir_inuse` | drive is mounted/active |
| 0x2000 | `curdir_splice` | JOIN or SUBST involved |
| 0x1000 | `curdir_local` | local FAT drive |

**Critical**: set `curdir_isnet | curdir_inuse` (`0xC000`), not just `curdir_isnet`. FreeDOS works with isnet alone, but **MS-DOS 4 silently treats isnet-without-inuse as "drive not present"** and never dispatches redirector calls. We learned this the hard way.

**SDA (Swappable Data Area)** — kernel scratch area updated on every DOS call. Returned by `INT 21h AX=5D06h`. Most of what your redirector reads on a per-call basis comes from here:

| Offset | Field | Purpose |
|--------|---|---|
| 0x0C   | DTA pointer (DWORD)         | user's buffer for Read (NOT DS:DX) |
| 0x9E   | qualified path (128 bytes)  | the canonicalized path DOS wants you to operate on |
| 0x19E  | sda_tmp_dm (21-byte SDB)    | search descriptor block during FindFirst/Next |
| 0x1B3  | SearchDir (32-byte FAT entry) | the entry you fill on a successful FindFirst |
| 0x24D  | SAttr                       | search attribute mask |

SDA offsets are pretty stable across DOS 3+ versions but the size grows slightly in DOS 4+. `INT 21h AX=5D06h` returns a length you can trust.

**SFT (System File Table)** entry — 64 bytes per open file, kernel's per-handle bookkeeping. DOS passes you `ES:DI = SFT pointer` on every Open / Read / Close call, and that pointer is *stable for the lifetime of the open file* — use it as the key to your own per-open state. (We do this in `tools/tsr.c`'s 8-slot `g_open[]` table.)

Important SFT offsets:

| Offset | Field |
|--------|---|
| 0x00 | `sf_ref_count` (word) — must be 1 on Open success, or DOS rejects the SFT |
| 0x02 | `sf_open_mode` (word) — kernel pre-sets the access bits before calling you |
| 0x04 | `sf_attr` (byte) |
| 0x05 | `sf_devinfo` (word) — bit 15 = network/redirector; bits 0-7 = drive index |
| 0x0D | time / 0x0F date / 0x11 size (dword) / 0x15 position (dword) |
| 0x20 | filename (11 bytes 8.3) |

For redirector-owned files: set `sf_devinfo = 0x8000 | drive_index` and `sf_ref_count = 1`. Forgetting either makes DOS think the open failed.

---

## 2. INT 2Fh AH=11h subfunction map

The DOS kernel issues these to your hooked INT 2Fh handler. Numbers are in decimal *and* hex because some sources use one and some use the other — confusing.

### Standard (DOS 3+) — these are the ones FreeDOS issues

| AL | Hex | Name | When DOS calls it |
|----|-----|---|---|
| 5  | 05h | ChDir | `CD Y:\foo` |
| 6  | 06h | Close | `INT 21h AH=3Eh` on a redirector handle |
| 7  | 07h | Commit File | `INT 21h AH=68h` flush |
| 8  | 08h | Read | `INT 21h AH=3Fh` on a redirector handle |
| 9  | 09h | Write | (write support; not in our v1) |
| 0Ah | 0Ah | Lock Region | |
| 0Bh | 0Bh | Unlock Region | |
| 0Ch | 0Ch | Get Disk Space | `INT 21h AH=36h` (DIR's free-space line) |
| 0Eh | 0Eh | Set File Attributes | |
| 0Fh | 0Fh | Get File Attributes | |
| 11h | 11h | Rename | |
| 13h | 13h | Delete | |
| 16h | 16h | Open Existing File | `INT 21h AH=3Dh` (the common path) |
| 17h | 17h | Create | `INT 21h AH=3Ch` |
| 1Bh | 1Bh | FindFirst | `INT 21h AH=4Eh` for redirector drive |
| 1Ch | 1Ch | FindNext | `INT 21h AH=4Fh` |
| 1Dh | 1Dh | FindClose / IFS_ABORT | DOS done iterating, releases search state |
| 21h | 21h | LSeek | `INT 21h AH=42h` on redirector handle |
| 22h | 22h | Process Termination Hook | called when a process holding redirector handles exits |
| 23h | 23h | Qualify Remote File Name | DOS asks you to canonicalize a path (TRUENAME) |

### MS-DOS 4 IFS-specific additions

MS-DOS 4 was Microsoft's first stab at a generic Installable File System layer. It uses INT 2Fh AH=11h with a *different* set of AL values for some operations:

| AL | Hex | Name | Notes |
|----|-----|---|---|
| 15h | 15h | IFS_OPEN (handle-based) | parallel to AL=0x16 SEQ_OPEN |
| 19h | 19h | IFS_SEQ_SEARCH_FIRST | **MS-DOS 4 dispatches FindFirst here, not 1Bh** |
| 1Ah | 1Ah | IFS_SEQ_SEARCH_NEXT | parallel to 1Ch |
| 2Dh | 2Dh | Get/Set Extended Attributes | dispatched from `INT 21h AH=57h AL=2..4` (FileTimes) — **TYPE calls 5702h after Open to read code-page tag; if you don't claim 2Dh, the kernel returns "Invalid function" and TYPE prints a misleading error** |
| 2Eh | 2Eh | Extended Open | dispatched from `INT 21h AX=6C00h` (and *all* TYPE opens go through here on MS-DOS 4) |
| A3h | A3h | Get Large Free Space (FreeDOS 32-bit ext.) | future: handle this for full disk sizes without 16-bit cluster scaling |

If you don't recognize a subfunction, **chain to `prev_int2f` rather than returning an error**. We learned the hard way that returning ERROR_FILE_NOT_FOUND for unknown AL values confuses MS-DOS 4's path-resolution / file-info plumbing.

### The Qualify (AX=1123h) handler contract — easy to get wrong

DOS calls AX=1123h to canonicalize a path. Inputs: `DS:SI = source path`, `ES:DI = 67-byte output buffer`. Returns `CF=0` (you canonicalized, output is in ES:DI) or `CF=1` (DOS does it locally).

**Two ways we got this wrong before getting it right:**

1. Returning `CF=0` unconditionally with empty `ES:DI`. FreeDOS shrugs and uses the original path. **MS-DOS 4 takes the empty buffer literally** and the subsequent OPEN of garbage fails. Visible symptom: SYSINIT bails with "Bad or missing Command Interpreter."

2. Returning `CF=0` only for our drive letter but still not writing `ES:DI`. Same garbage-buffer problem, same result.

**Right answer:**

```c
if (path_starts_with("Y:") /* case-insensitive */) {
    copy_path_into_es_di();
    r.w.flags &= ~1u;  /* CF=0 */
} else {
    r.w.flags |= 1u;   /* CF=1, DOS handle it */
}
```

---

## 3. Toolchain gotchas — OpenWatcom small-model TSRs

### `_dos_keep` paragraph count must come from the MCB, never hard-coded

```c
uint8_t  __far *mcb       = (uint8_t  __far *)MK_FP(_psp - 1, 0);
uint16_t __far *mcb_paras = (uint16_t __far *)(mcb + 3);
_dos_keep(0, *mcb_paras);
```

If you hard-code a paragraph count, you'll either:
- Pick too small → DOS silently truncates your BSS / static buffers; statics at the high end of your segment land in freed memory; DOS overwrites them later. Symptom: mount succeeds at install time (banner shows correct values) but afterward `bd->sector_size == 0`, FindFirst returns "file not found" for everything, DIR shows zero entries. *Devious to diagnose because the corruption only kicks in after `_dos_keep` returns.*
- Pick too big → some DOSes are picky (MS-DOS 4 specifically gets confused by extra-big resident blocks).

The MCB allocation is what DOS *actually* gave you. Use it.

### `SS != DS` in interrupt handlers

When DOS calls your INT 2Fh hook, the kernel's stack is active — `SS` is the kernel's stack segment, *not* your DGROUP. Watcom's `__interrupt` prologue sets `DS` to your DGROUP for you, so static globals via near pointers work. But:

- **Stack-local variables live in SS**, not DS. If any code accessing them assumes DS-relative addressing (e.g., `&local_var` becomes a near pointer that gets dereferenced through the wrong segment), you read/write random kernel memory.
- **The `bdev_read` path** stores the DAP buffer's segment as DS internally. A stack-local DAP would be unreachable. **Fix: declare every block-I/O-related local as `static`** so it lives in DGROUP.
- **64-bit math is broken** in handler context with Watcom's small model — use `uint32_t` for sector/byte arithmetic and avoid `uint64_t` multiplications.

The pattern that bit us multiple times: a function that worked fine when called from `main()` failed mysteriously when called from inside the INT 2Fh handler. Always: locals → `static`.

### Avoid `malloc` on the DOS side

The Watcom heap lives at the high end of your segment, exactly the area most exposed to undersized-`_dos_keep` truncation. Even if you size keep correctly, malloc'd memory has no advantage over static singletons for a TSR. Use `static` everywhere; allocate once at startup or use fixed pools.

### Watcom builds: `armo64`, `bino64`, `binl64`

OpenWatcom ships per-host binaries in different subdirs:

- `$WATCOM/armo64/` — Apple Silicon (macOS arm64)
- `$WATCOM/bino64/` — Intel macOS
- `$WATCOM/binl64/` — Linux x86_64

Our Makefile auto-detects via `uname -s` / `uname -m`; override with `WATCOM_BIN=` if your install lives elsewhere.

---

## 4. DOS-version-specific quirks

### FreeDOS 1.x

- LFN: kernel has stub-level support for `INT 21h AX=71xxh` — only `71A6h` (Get File Info by Handle) actually dispatches to the redirector. Practical LFN for redirector drives requires DOSLFN.COM or equivalent third-party TSR; even then, no documented protocol for asking redirectors about long names.
- Lenient about Qualify-handler bugs and about CDS flag-bit completeness — works with `curdir_isnet` alone (no `curdir_inuse`).
- Default INT 21h calls are dispatched by AL number that matches RBIL "standard" (1Bh for FindFirst, 16h for Open, etc).

### MS-DOS 4.0 (April 2024 open-source release)

- IFSFUNC.EXE is **not needed and actively harmful** if loaded without IFS drivers. It prints "Invalid configuration" (UTIL_ERR_4) and leaves the kernel half-initialized — this is what causes the misleading "Bad or missing Command Interpreter" cascade if you naively do `INSTALL=A:\IFSFUNC.EXE`.
- TSRs that call `_dos_keep` from inside an AUTOEXEC.BAT batch context corrupt COMMAND.COM's batch-position tracking → "Insert disk with batch file" prompt and stalled boot. **Solution: load via `CONFIG.SYS INSTALL=`**, before COMMAND.COM and AUTOEXEC.BAT exist.
- Uses different INT 2Fh AH=11h subfunction numbers for several operations (see table above): IFS_SEQ_SEARCH_FIRST=0x19, Extended Open=0x2E, Get/Set XA=0x2D.
- TYPE in MS-DOS 4 calls `INT 21h AX=5702h` immediately after Open (to read the file's code-page tag). That dispatches to redirector AL=0x2D. **Skipping AL=0x2D causes TYPE to print "Invalid function" — the file *is* opened, but the kernel returns error to the caller before the read.**
- DIR's "bytes free" display is buggy when the redirector returns scaled-up cluster sizes — see Known Limitations below.
- **TSR size sensitivity (RESOLVED 2026-05-02).** *Symptom:* adding ~6 bytes of `static const` data to our DGROUP made `ext4_path_lookup("/hello.txt")` return 0 on the second Open. *Root cause:* MS-DOS 4's CDS slot has IFS fields past the 67-byte path (`curdir_type` at +0x51, `curdir_ifs_hdr` far ptr at +0x52). We were only zeroing the first 67 bytes, so the IFS-header pointer held whatever stale kernel state was there. Because MS-DOS 4 uses the same bit value (0x8000) for `curdir_isnet` and `curdir_isifs`, the kernel followed our `curdir_ifs_hdr` garbage, loaded a segment register from the dereferenced "IFS file system header", and wrote into our DGROUP — specifically a `mov [bx],dx` in the FAT time-encoder routine that landed in `g_fs.sb.block_size`. Every subsequent ext4 read then computed sectors against a corrupt block_size and INT 13h failed. The bug was layout-sensitive only in *which* of our static fields sat at the corrupted DGROUP offset. *Fix:* `hook_cds()` now zeroes all 88 bytes of the slot before populating it (commit at the same time as this doc edit). Found via DOSBox-X heavy debugger BPM trap on `&g_fs.sb.block_size`; the trap fired in MS-DOS 4 kernel code at `CS:0F30` with `DS=` our DGROUP, `BX=08C4`, `DX=0x08B8` (= the DOS-packed mtime of `HELLO.TXT`).
- The MIT-licensed source at `references/msdos4/v4.0/src/` is the only authoritative reference for these behaviors. Particularly useful: `src/DOS/OPEN.ASM`, `src/DOS/HANDLE.ASM`, `src/DOS/ISEARCH.ASM` (kernel dispatch points), `src/INC/CURDIR.INC` (CDS layout + flag bits), `src/INC/mult.inc` (subfunction numbers), `src/CMD/IFSFUNC/` (IFSFUNC source — *not needed*, but useful for understanding what a real IFS driver does).

---

## 5. Known limitations and workarounds

### Multi-file open

Default DOS `FILES=` defaults to 8; we have an 8-slot SFT-keyed table. If you need more, raise `FILES=` in CONFIG.SYS *and* bump `MAX_OPEN_SLOTS` in `tools/tsr.c`.

### GetDiskSpace and the MS-DOS 4 DIR-free-space quirk

DOS's `INT 21h AH=36h` returns four 16-bit values. For ext4 disks > 65535 blocks we'd lose precision unless we scale up `sectors-per-cluster` and proportionally scale down the cluster counts. Total bytes stays the same.

**However**, MS-DOS 4's DIR command has a buggy free-space *display* path that mishandles the scaled return values for disks past ~96 MB. The redirector returns correct numbers (FreeDOS DIR shows them right); only MS-DOS 4 DIR's printed `bytes free` is wrong. **Actual file operations are unaffected.**

For now we use a conservative threshold (truncate up to ~96 MB; scale beyond) so the common small-disk case keeps MS-DOS 4 happy. Future fix: implement `INT 2Fh AX=11A3h` (Get Extended Free Space) so callers that opt in get exact 32-bit precision regardless.

### LFN

Long filenames (`AX=71xxh`) only work meaningfully on FreeDOS-with-DOSLFN, and DOSLFN is FAT-only — it can't query our redirector for long names. We currently truncate ext4 names to 8.3 with a (TODO) collision-suffix generator. Expect to see `MYREAL~1.TXT` for `myreallylongfilename.txt`.

### Write support

Read-only for v1. Writes are deliberately out of scope — `CREATE`, `WRITE`, `DELETE`, `RENAME`, `SET_ATTR`, etc. all return error.

---

## 6. References — what we actually used

The "study these end-to-end" list:

- **MS-DOS 4 source**: `references/msdos4/v4.0/src/` — Microsoft's MIT-licensed v4.0 release. Indispensable for understanding kernel dispatch (especially the IFS-specific subfunctions). Cloned via `git clone https://github.com/microsoft/MS-DOS.git references/msdos4`.
- **FreeDOS kernel source**: `references/freedos-kernel/` — git clone of the FreeDOS kernel for ground-truth on SDA / CDS layout and the redirector dispatch path.
- **Ralph Brown's Interrupt List (RBIL)**: the canonical historical reference for DOS API surfaces. Not always accurate on register-order details for AH=11h subfunctions — verify against actual DOS source when in doubt.
- **lwext4** (`https://github.com/gkostka/lwext4`, BSD): cleanest open-source ext4 reader. We don't vendor or link, but we cross-reference its handling of extents and directories.
- **GRUB `grub-core/fs/ext2.c`** (GPLv3): battle-tested ext2/3/4 read path. Reference only; not vendored.
- **Linux kernel ext4 docs**: `https://www.kernel.org/doc/html/latest/filesystems/ext4/` — the authoritative on-disk format spec.

Don't vendor any of these — keep them as references the user clones into `references/` (which is gitignored) when needed.
