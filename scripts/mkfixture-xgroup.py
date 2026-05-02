#!/usr/bin/env python3
"""Fixture for phase 2.5 cross-group allocation test.

Creates an ext4 with small blocks_per_group (-g 256) so that a 16 MiB
FS has ~64 groups. A ~220 KiB filler packs group 0's data blocks full.
/target.txt gets one block in group 0. Extending it forces allocation
into group 1+. We also manually set BLOCK_UNINIT in group 1's bg_flags
to exercise the uninit-bitmap initialization path (without GDT_CSUM,
mkfs.ext4 on macOS doesn't naturally set BLOCK_UNINIT; we force it).
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT       = Path(__file__).resolve().parent.parent
IMG        = ROOT / "tests/images/xgroup.img"
BLOCK_SIZE = 1024
BPG        = 256   # blocks_per_group (small so 16 MiB has ~64 groups)
FS_SIZE_MB = 16    # larger to avoid minimum-size refusals

MKFS_CANDIDATES = [
    "mkfs.ext4",
    "/opt/homebrew/opt/e2fsprogs/sbin/mkfs.ext4",
    "/usr/local/opt/e2fsprogs/sbin/mkfs.ext4",
    "/sbin/mkfs.ext4",
]

def find_mkfs():
    found = shutil.which("mkfs.ext4")
    if found: return found
    for p in MKFS_CANDIDATES:
        if os.path.exists(p): return p
    sys.exit("mkfs.ext4 not found")


def main():
    IMG.parent.mkdir(parents=True, exist_ok=True)
    if IMG.exists(): IMG.unlink()
    subprocess.run(["truncate", "-s", f"{FS_SIZE_MB}M", str(IMG)], check=True)

    with tempfile.TemporaryDirectory() as tdir:
        # Filler: enough to pack group 0's data area. With bpg=256,
        # group 0 has 256 blocks total. ~20 are reserved (SB, GDT,
        # bitmaps, inode table). ~236 are data. Use 230 KiB to leave
        # only a few slots free.
        with open(os.path.join(tdir, "filler.bin"), "wb") as f:
            f.write(b"F" * (230 * BLOCK_SIZE))

        with open(os.path.join(tdir, "target.txt"), "wb") as f:
            f.write(b"A" * BLOCK_SIZE)

        subprocess.run(
            [find_mkfs(), "-F",
             "-b", str(BLOCK_SIZE),
             "-g", str(BPG),
             "-O", "^metadata_csum,has_journal",
             "-N", "32",
             "-L", "ext4-xgroup",
             "-d", tdir,
             str(IMG)],
            check=True,
            stdout=subprocess.DEVNULL,
        )

    print(f"Wrote fixture: {IMG} ({FS_SIZE_MB} MiB, bpg={BPG}) "
          f"with filler.bin (fills group 0) + /target.txt")


if __name__ == "__main__":
    main()
