#!/usr/bin/env python3
"""Fixture for in-place file-write tests.

A clean ext4 with metadata_csum disabled (the write path requires
metadata_csum-enabled fixtures to use mkfixture-write-csum.py instead),
has_journal, and a single known file `/target.txt` filled with one block
of 'A'.

The test (host_write_test) writes new content (one block of 'B') via
ext4_file_write_block and verifies the read-back matches.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT       = Path(__file__).resolve().parent.parent
IMG        = ROOT / "tests/images/write.img"
FS_SIZE_MB = 8
BLOCK_SIZE = 1024  # smaller blocks → simpler arithmetic + smaller buffers in DOS

MKFS_CANDIDATES = [
    "mkfs.ext4",
    "/opt/homebrew/opt/e2fsprogs/sbin/mkfs.ext4",
    "/usr/local/opt/e2fsprogs/sbin/mkfs.ext4",
    "/sbin/mkfs.ext4",
]


def find_mkfs():
    found = shutil.which("mkfs.ext4")
    if found:
        return found
    for p in MKFS_CANDIDATES:
        if os.path.exists(p):
            return p
    sys.exit("mkfs.ext4 not found")


def main():
    IMG.parent.mkdir(parents=True, exist_ok=True)
    if IMG.exists():
        IMG.unlink()
    subprocess.run(["truncate", "-s", f"{FS_SIZE_MB}M", str(IMG)], check=True)

    with tempfile.TemporaryDirectory() as tdir:
        # One block of 'A' — same length as our future 'B' block, in-place.
        with open(os.path.join(tdir, "target.txt"), "wb") as f:
            f.write(b"A" * BLOCK_SIZE)
        # A second known file so the test has something to verify
        # _wasn't_ touched by the write path.
        with open(os.path.join(tdir, "control.txt"), "wb") as f:
            f.write(b"unchanged\n")

        subprocess.run(
            [find_mkfs(), "-F",
             "-b", str(BLOCK_SIZE),
             "-O", "^metadata_csum,has_journal",
             "-N", "64",
             "-L", "ext4-dos-write",
             "-d", tdir,
             str(IMG)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
    print(f"Wrote fixture: {IMG} ({FS_SIZE_MB} MiB) "
          f"with /target.txt (1 block 'A') + /control.txt")


if __name__ == "__main__":
    main()
