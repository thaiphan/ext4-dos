#!/usr/bin/env python3
"""Like mkfixture-write.py but with metadata_csum on (default
mkfs.ext4 features), so phase 1c's inode-checksum recompute path is
exercised end-to-end and e2fsck can validate the post-write image.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT       = Path(__file__).resolve().parent.parent
IMG        = ROOT / "tests/images/write-csum.img"
FS_SIZE_MB = 8
BLOCK_SIZE = 1024

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
    if IMG.exists():
        IMG.unlink()
    subprocess.run(["truncate", "-s", f"{FS_SIZE_MB}M", str(IMG)], check=True)

    with tempfile.TemporaryDirectory() as tdir:
        with open(os.path.join(tdir, "target.txt"), "wb") as f:
            f.write(b"A" * BLOCK_SIZE)
        with open(os.path.join(tdir, "control.txt"), "wb") as f:
            f.write(b"unchanged\n")

        # Default mkfs.ext4 features: metadata_csum on, has_journal on.
        subprocess.run(
            [find_mkfs(), "-F",
             "-b", str(BLOCK_SIZE),
             "-N", "64",
             "-L", "ext4-dos-wcsum",
             "-d", tdir,
             str(IMG)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
    print(f"Wrote fixture: {IMG} ({FS_SIZE_MB} MiB) "
          f"with /target.txt + /control.txt, metadata_csum on")


if __name__ == "__main__":
    main()
