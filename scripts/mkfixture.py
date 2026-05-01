#!/usr/bin/env python3
"""Create a bare ext4 fixture (no partition table) populated with known files.

Default 16 MiB. Override with SIZE_MB env var.

Files inside the FS:
  /hello.txt          ASCII contents (canonical test data)
  /subdir/nested.txt  Smaller ASCII file in a subdirectory
"""
import os
import shutil
import subprocess
import sys
import tempfile

OUT_DIR = "tests/images"
OUT_PATH = f"{OUT_DIR}/small.img"
SIZE_MB = int(os.environ.get("SIZE_MB", "16"))

HELLO_BYTES = b"Hello, ext4-dos!\nThis is a test file used by tools/host_cli.\n"
NESTED_BYTES = b"Nested file contents.\n"

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

    with tempfile.TemporaryDirectory() as tdir:
        with open(os.path.join(tdir, "hello.txt"), "wb") as f:
            f.write(HELLO_BYTES)
        os.makedirs(os.path.join(tdir, "subdir"), exist_ok=True)
        with open(os.path.join(tdir, "subdir", "nested.txt"), "wb") as f:
            f.write(NESTED_BYTES)

        subprocess.check_call(
            [mkfs, "-F", "-L", "ext4-dos-test", "-d", tdir, OUT_PATH],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    print(f"Wrote fixture: {OUT_PATH} ({SIZE_MB} MiB) with /hello.txt + /subdir/nested.txt")

if __name__ == "__main__":
    main()
