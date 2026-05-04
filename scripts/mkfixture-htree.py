#!/usr/bin/env python3
"""Build a fixture with an htree-indexed directory, for testing the
htree-aware CREATE path (src/ext4/htree.c).

Steps:
  1. Pre-populate a tempdir with /htreedir/fileNNN.txt for N = 0..149.
     Each name is short; with 1 KiB blocks an entry costs ~24 bytes,
     so ~40 fit per block. 150 entries fill ~4 leaf blocks once
     converted to htree.
  2. mkfs.ext4 -b 1024 -F -d tdir IMG — initial layout is linear
     (mkfs's -d copies entries directly without going through the
     kernel's per-write htree-conversion logic).
  3. e2fsck -fyD IMG — the -D flag tells e2fsck to "optimize"
     directories: dirs above one block get converted to htree, with
     a real dx_root + leaf blocks. This is the one-shot path to
     produce an htree dir without root or kernel cooperation.

The host_htree_test test then mounts a writable copy of IMG, asserts
that /htreedir's i_flags has EXT2_INDEX_FL set, creates a new file
inside it (exercising ext4_htree_find_leaf + leaf insert), and asserts
e2fsck -fn still reports clean afterwards.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT       = Path(__file__).resolve().parent.parent
IMG        = ROOT / "tests/images/htree.img"
FS_SIZE_MB = 8
BLOCK_SIZE = 1024
N_ENTRIES  = 150

TOOL_CANDIDATES = {
    "mkfs.ext4": [
        "mkfs.ext4",
        "/opt/homebrew/opt/e2fsprogs/sbin/mkfs.ext4",
        "/usr/local/opt/e2fsprogs/sbin/mkfs.ext4",
        "/sbin/mkfs.ext4",
    ],
    "e2fsck": [
        "e2fsck",
        "/opt/homebrew/opt/e2fsprogs/sbin/e2fsck",
        "/usr/local/opt/e2fsprogs/sbin/e2fsck",
        "/sbin/e2fsck",
    ],
}


def find_tool(name: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    for p in TOOL_CANDIDATES[name]:
        if os.path.exists(p):
            return p
    sys.exit(f"{name} not found (brew install e2fsprogs / apt install e2fsprogs)")


def main():
    IMG.parent.mkdir(parents=True, exist_ok=True)
    if IMG.exists():
        IMG.unlink()
    subprocess.run(["truncate", "-s", f"{FS_SIZE_MB}M", str(IMG)], check=True)

    mkfs   = find_tool("mkfs.ext4")
    e2fsck = find_tool("e2fsck")

    with tempfile.TemporaryDirectory() as tdir:
        htree_dir = os.path.join(tdir, "htreedir")
        os.makedirs(htree_dir)
        for i in range(N_ENTRIES):
            with open(os.path.join(htree_dir, f"file{i:03d}.txt"), "wb") as f:
                f.write(f"entry {i}\n".encode())
        # A control file in the root that we won't touch.
        with open(os.path.join(tdir, "control.txt"), "wb") as f:
            f.write(b"unchanged\n")

        subprocess.run(
            [mkfs, "-F",
             "-b", str(BLOCK_SIZE),
             "-O", "^metadata_csum,has_journal,dir_index",
             "-N", "256",
             "-L", "ext4-dos-htree",
             "-d", tdir,
             str(IMG)],
            check=True,
            stdout=subprocess.DEVNULL,
        )

    # Convert /htreedir from linear to htree. e2fsck -fyD does exactly
    # this when dir_index is enabled in features. Exits 1 on "fixed";
    # exits 0 on "no changes needed" — both fine for our purposes.
    rc = subprocess.run([e2fsck, "-fyD", str(IMG)],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
    if rc not in (0, 1):
        sys.exit(f"e2fsck -fyD failed with rc={rc}")

    # Final clean check.
    rc = subprocess.run([e2fsck, "-fn", str(IMG)],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
    if rc != 0:
        sys.exit(f"e2fsck -fn on finished image returned rc={rc}")

    print(f"Wrote fixture: {IMG} ({FS_SIZE_MB} MiB) "
          f"with /htreedir of {N_ENTRIES} files (htree-indexed)")


if __name__ == "__main__":
    main()
