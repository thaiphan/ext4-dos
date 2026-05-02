#!/usr/bin/env python3
"""Create a partitioned disk image with one ext4 Linux partition, populated
with the same test files as the bare fixture.

Layout: 1 MiB unused (MBR + alignment), then ext4 partition to end of file.
Default 64 MiB. Override with SIZE_MB env var.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

OUT_DIR = "tests/images"
OUT_PATH = f"{OUT_DIR}/disk.img"
SIZE_MB = int(os.environ.get("SIZE_MB", "64"))
SECTOR_SIZE = 512
PART_START_LBA = 2048

HELLO_BYTES = b"Hello, ext4-dos!\nThis is a test file used by tools/host_cli.\n"
NESTED_BYTES = b"Nested file contents.\n"
LONG_NAME_1 = "verylongname1.txt"
LONG_NAME_2 = "verylongname2.txt"
LONG_BYTES_1 = b"long-named file ONE.\n"
LONG_BYTES_2 = b"long-named file TWO.\n"
# 1024-byte target.txt: same length as one FS block, so phase 1b's
# strict in-place writer can overwrite it from DOS.
TARGET_BYTES = b"A" * 1024

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

    with tempfile.TemporaryDirectory() as tdir:
        with open(os.path.join(tdir, "hello.txt"), "wb") as f:
            f.write(HELLO_BYTES)
        os.makedirs(os.path.join(tdir, "subdir"), exist_ok=True)
        with open(os.path.join(tdir, "subdir", "nested.txt"), "wb") as f:
            f.write(NESTED_BYTES)
        # Two long-named files sharing a 4-char prefix — exercises the
        # hash-based ~XXX alias generator (Win95-style 8.3 alias).
        with open(os.path.join(tdir, LONG_NAME_1), "wb") as f:
            f.write(LONG_BYTES_1)
        with open(os.path.join(tdir, LONG_NAME_2), "wb") as f:
            f.write(LONG_BYTES_2)
        # /target.txt is exactly 1 block — used by phase 1b's DOS write test.
        with open(os.path.join(tdir, "target.txt"), "wb") as f:
            f.write(TARGET_BYTES)

        offset_bytes = PART_START_LBA * SECTOR_SIZE
        size_1k_blocks = (part_sector_count * SECTOR_SIZE) // 1024
        # Default features (incl. metadata_csum) — phase 1c recomputes
        # inode i_checksum on writes through ext4_inode_recompute_csum.
        subprocess.check_call(
            [
                mkfs, "-F", "-b", "1024",
                "-E", f"offset={offset_bytes}",
                "-L", "ext4-dos-part",
                "-d", tdir,
                OUT_PATH, str(size_1k_blocks),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    print(f"Wrote partitioned fixture: {OUT_PATH} "
          f"({SIZE_MB} MiB, partition at LBA {PART_START_LBA}) with /hello.txt, "
          f"/{LONG_NAME_1}, /{LONG_NAME_2}, /subdir/nested.txt, /target.txt")

if __name__ == "__main__":
    main()
