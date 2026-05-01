#!/usr/bin/env python3
"""Create a bare ext4 fixture (no partition table). Default 16 MiB."""
import os
import shutil
import subprocess
import sys

OUT_DIR = "tests/images"
OUT_PATH = f"{OUT_DIR}/small.img"
SIZE_MB = int(os.environ.get("SIZE_MB", "16"))

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
    with open(OUT_PATH, "wb") as f:
        f.truncate(SIZE_MB * 1024 * 1024)
    subprocess.check_call(
        [mkfs, "-F", "-L", "ext4-dos-test", OUT_PATH],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    print(f"Wrote fixture: {OUT_PATH} ({SIZE_MB} MiB)")

if __name__ == "__main__":
    main()
