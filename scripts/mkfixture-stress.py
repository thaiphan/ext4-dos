#!/usr/bin/env python3
"""Build a 'realistic' ext4 fixture for v1 verification.

What this exercises that the basic 64 MB fixture doesn't:
- 4 KiB blocks (mkfs.ext4 default; basic fixture uses 1 KiB)
- Modern feature set: huge_file, dir_nlink, extra_isize, metadata_csum,
  dir_index — all on by default in current mkfs.ext4
- 500+ files in /many (exercises htree directories)
- 4 MB binary file (multi-block read across multiple extents)
- Nested directories 3 deep

Used by `make host-stress` — host_cli reads representative files and
asserts content matches.

Default 256 MiB. Override with SIZE_MB env var.
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

OUT_DIR  = "tests/images"
OUT_PATH = f"{OUT_DIR}/stress.img"
SIZE_MB  = int(os.environ.get("SIZE_MB", "256"))
SECTOR_SIZE    = 512
PART_START_LBA = 2048

MKFS_FALLBACKS = [
    "/opt/homebrew/opt/e2fsprogs/sbin/mkfs.ext4",
    "/usr/local/opt/e2fsprogs/sbin/mkfs.ext4",
]

def find_mkfs() -> str:
    found = shutil.which("mkfs.ext4")
    if found:
        return found
    for c in MKFS_FALLBACKS:
        if os.access(c, os.X_OK):
            return c
    print("ERROR: mkfs.ext4 not found (brew install e2fsprogs / apt install e2fsprogs)",
          file=sys.stderr)
    sys.exit(1)

def main() -> None:
    mkfs = find_mkfs()
    os.makedirs(OUT_DIR, exist_ok=True)
    if os.path.exists(OUT_PATH):
        os.remove(OUT_PATH)

    total_bytes  = SIZE_MB * 1024 * 1024
    total_secs   = total_bytes // SECTOR_SIZE
    part_secs    = total_secs - PART_START_LBA

    with open(OUT_PATH, "wb") as f:
        f.truncate(total_bytes)

    # MBR with one Linux partition.
    mbr = bytearray(512)
    entry = struct.pack(
        "<BBBBBBBBII",
        0x00, 0x00, 0x02, 0x00, 0x83, 0xFE, 0xFF, 0xFF,
        PART_START_LBA, part_secs,
    )
    mbr[446:446 + 16] = entry
    mbr[510] = 0x55
    mbr[511] = 0xAA
    with open(OUT_PATH, "r+b") as f:
        f.write(mbr)

    with tempfile.TemporaryDirectory() as tdir:
        # /readme.txt — sentinel file the host stress test will TYPE.
        with open(os.path.join(tdir, "readme.txt"), "wb") as f:
            f.write(b"stress fixture for ext4-dos v1 verification\n")

        # /big.bin — 4 MB pseudorandom; tests multi-block + multi-extent reads.
        # Also stamp a SHA-256 marker file so host stress test can verify.
        big_bytes = b"".join(
            hashlib.sha256(f"stress-block-{i}".encode()).digest() * 32
            for i in range(4096)        # 4096 * 32 * 32 = 4 MB
        )
        with open(os.path.join(tdir, "big.bin"), "wb") as f:
            f.write(big_bytes)
        big_sha = hashlib.sha256(big_bytes).hexdigest()
        with open(os.path.join(tdir, "big.sha256"), "w") as f:
            f.write(big_sha + "\n")

        # /many/* — 500 small files, exercises htree.
        many_dir = os.path.join(tdir, "many")
        os.makedirs(many_dir)
        for i in range(500):
            with open(os.path.join(many_dir, f"file{i:04d}.txt"), "wb") as f:
                f.write(f"file {i}\n".encode())

        # /a/b/c/deep.txt — exercises path walk depth.
        deep_dir = os.path.join(tdir, "a", "b", "c")
        os.makedirs(deep_dir)
        with open(os.path.join(deep_dir, "deep.txt"), "wb") as f:
            f.write(b"deep!\n")

        offset_bytes = PART_START_LBA * SECTOR_SIZE
        size_blocks  = (part_secs * SECTOR_SIZE) // 4096
        # mkfs.ext4 with DEFAULT block size (4 KiB) and DEFAULT features —
        # this is what current Linux distros produce.
        subprocess.check_call(
            [
                mkfs, "-F", "-b", "4096",
                "-E", f"offset={offset_bytes}",
                "-L", "ext4-dos-stress",
                "-d", tdir,
                OUT_PATH, str(size_blocks),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    print(f"Wrote stress fixture: {OUT_PATH} ({SIZE_MB} MiB)")
    print(f"  /big.bin sha256 = {big_sha}")
    print(f"  /many/ has 500 files")
    print(f"  /a/b/c/deep.txt nested 3 deep")

if __name__ == "__main__":
    main()
