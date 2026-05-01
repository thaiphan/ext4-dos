#!/usr/bin/env python3
"""Create a partitioned disk image with one ext4 Linux partition.

Layout: 1 MiB unused (MBR + alignment), then ext4 partition to end of file.
Default 64 MiB. Override with SIZE_MB env var.
"""
import os
import shutil
import struct
import subprocess
import sys

OUT_DIR = "tests/images"
OUT_PATH = f"{OUT_DIR}/disk.img"
SIZE_MB = int(os.environ.get("SIZE_MB", "64"))
SECTOR_SIZE = 512
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
    print("ERROR: mkfs.ext4 not found.", file=sys.stderr)
    print("  macOS:     brew install e2fsprogs", file=sys.stderr)
    print("  Debian:    apt install e2fsprogs", file=sys.stderr)
    print("  Fedora:    dnf install e2fsprogs", file=sys.stderr)
    print("  Arch:      pacman -S e2fsprogs", file=sys.stderr)
    print("  MSYS2:     pacman -S e2fsprogs", file=sys.stderr)
    print("  WSL2:      apt install e2fsprogs", file=sys.stderr)
    sys.exit(1)

def main() -> None:
    mkfs = find_mkfs()
    os.makedirs(OUT_DIR, exist_ok=True)
    if os.path.exists(OUT_PATH):
        os.remove(OUT_PATH)

    total_bytes = SIZE_MB * 1024 * 1024
    total_sectors = total_bytes // SECTOR_SIZE
    part_sector_count = total_sectors - PART_START_LBA

    with open(OUT_PATH, "wb") as f:
        f.truncate(total_bytes)

    mbr = bytearray(512)
    entry = struct.pack(
        "<BBBBBBBBII",
        0x00,                # status
        0x00, 0x02, 0x00,    # CHS first  (dummy)
        0x83,                # type: Linux
        0xFE, 0xFF, 0xFF,    # CHS last   (dummy)
        PART_START_LBA,
        part_sector_count,
    )
    mbr[446:446 + 16] = entry
    mbr[510] = 0x55
    mbr[511] = 0xAA
    with open(OUT_PATH, "r+b") as f:
        f.write(mbr)

    offset_bytes = PART_START_LBA * SECTOR_SIZE
    size_1k_blocks = (part_sector_count * SECTOR_SIZE) // 1024
    subprocess.check_call(
        [
            mkfs, "-F", "-b", "1024",
            "-E", f"offset={offset_bytes}",
            "-L", "ext4-dos-part",
            OUT_PATH, str(size_1k_blocks),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    print(f"Wrote partitioned fixture: {OUT_PATH} "
          f"({SIZE_MB} MiB, partition at LBA {PART_START_LBA})")

if __name__ == "__main__":
    main()
