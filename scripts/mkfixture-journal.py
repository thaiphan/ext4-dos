#!/usr/bin/env python3
"""Build a fixture with a known dirty journal.

Steps:
  1. mkfs.ext4 a small image with metadata_csum disabled (so the journal
     also has no CSUM_V2/V3 — keeps the forged commit block valid even
     without computing CRC32C).
  2. Locate the journal inode and the root directory's data block.
  3. Forge a 1-transaction journal:
        descriptor (1 tag → root_data_block)
        data       (root data block bytes, first byte XOR 0x42)
        commit
     Set s_start, s_sequence accordingly and the RECOVER incompat bit
     on the FS superblock.

The test (host_journal_test) re-reads the expect sidecar and verifies
that the replay walker built a map containing exactly the forged entry.
"""

import os
import struct
import subprocess
import sys
from pathlib import Path

ROOT       = Path(__file__).resolve().parent.parent
IMG        = ROOT / "tests/images/journal.img"
EXPECT     = ROOT / "tests/images/journal.expect"
FS_SIZE_MB = 8

JBD_MAGIC  = 0xC03B3998
EXT4_MAGIC = 0xEF53
EXT4_RECOVER = 0x4

MKFS_CANDIDATES = [
    "mkfs.ext4",
    "/opt/homebrew/opt/e2fsprogs/sbin/mkfs.ext4",
    "/usr/local/opt/e2fsprogs/sbin/mkfs.ext4",
    "/sbin/mkfs.ext4",
]


def find_mkfs():
    import shutil
    found = shutil.which("mkfs.ext4")
    if found:
        return found
    for p in MKFS_CANDIDATES:
        if os.path.exists(p):
            return p
    print("ERROR: mkfs.ext4 not found", file=sys.stderr)
    sys.exit(1)


def le16(b, o): return struct.unpack_from("<H", b, o)[0]
def le32(b, o): return struct.unpack_from("<I", b, o)[0]
def be32(b, o): return struct.unpack_from(">I", b, o)[0]


def main():
    IMG.parent.mkdir(parents=True, exist_ok=True)
    if IMG.exists():
        IMG.unlink()
    subprocess.run(["truncate", "-s", f"{FS_SIZE_MB}M", str(IMG)], check=True)

    mkfs = find_mkfs()
    # Disable metadata_csum so the journal doesn't get CSUM_V2/V3 either
    # (forging a valid CRC32C is a separate phase). 1 KiB blocks keep
    # arithmetic small. -N 64 caps inodes to keep the inode table compact.
    subprocess.run(
        [mkfs, "-F",
         "-b", "1024",
         "-O", "^metadata_csum,has_journal",
         "-N", "64",
         "-L", "ext4-dos-jrnl",
         str(IMG)],
        check=True,
        stdout=subprocess.DEVNULL,
    )

    # ----- Read FS superblock -----
    with open(IMG, "rb") as f:
        f.seek(1024)
        sb = f.read(1024)

    if le16(sb, 0x38) != EXT4_MAGIC:
        sys.exit("not an ext4 image")

    block_size       = 1024 << le32(sb, 0x18)
    journal_inum     = le32(sb, 0xE0)
    inodes_per_group = le32(sb, 0x28)
    inode_size       = le16(sb, 0x58)
    feat_incompat    = le32(sb, 0x60)
    bgd_size         = 64 if (feat_incompat & 0x80) else 32

    # BGD table: at block 1 if block_size > 1024, else block 2.
    bgd_block = 1 if block_size > 1024 else 2

    # ----- BGD 0 -----
    with open(IMG, "rb") as f:
        f.seek(bgd_block * block_size)
        bgd0 = f.read(bgd_size)
    itab_lo = le32(bgd0, 0x08)
    itab_hi = le32(bgd0, 0x28) if bgd_size == 64 else 0
    itab    = (itab_hi << 32) | itab_lo

    # ----- Journal inode -----
    with open(IMG, "rb") as f:
        f.seek(itab * block_size + (journal_inum - 1) * inode_size)
        jinode = f.read(inode_size)
    i_block = jinode[0x28:0x28 + 60]
    if le16(i_block, 0) != 0xF30A:
        sys.exit("journal inode missing extent header")
    if le16(i_block, 6) != 0:
        sys.exit("journal inode extent depth != 0 (script assumes leaf-only)")
    n_ext = le16(i_block, 2)
    extents = []  # (logical, length, physical)
    for k in range(n_ext):
        e = i_block[12 + k * 12: 12 + (k + 1) * 12]
        e_log  = le32(e, 0)
        e_len  = le16(e, 4) & 0x7FFF
        e_phys = (le16(e, 6) << 32) | le32(e, 8)
        extents.append((e_log, e_len, e_phys))
    if extents[0][0] != 0:
        sys.exit("journal extent[0] doesn't start at logical 0")
    journal_phys = extents[0][2]  # phys of journal block 0 (the jsb)

    def jblk_to_phys(jblk):
        for (e_log, e_len, e_phys) in extents:
            if e_log <= jblk < e_log + e_len:
                return e_phys + (jblk - e_log)
        sys.exit(f"jblk {jblk} not covered by any extent")

    # ----- Journal superblock -----
    with open(IMG, "rb") as f:
        f.seek(journal_phys * block_size)
        jsb = bytearray(f.read(block_size))
    if be32(jsb, 0) != JBD_MAGIC:
        sys.exit("bad jbd magic")

    jsb_first = be32(jsb, 0x14)
    jsb_seq   = be32(jsb, 0x18)
    jsb_start = be32(jsb, 0x1C)
    jsb_feat  = be32(jsb, 0x28)
    jsb_uuid  = bytes(jsb[0x30:0x40])

    if jsb_start != 0:
        sys.exit("journal not clean before forging")
    if jsb_feat & 0x18:  # CSUM_V2 | CSUM_V3
        sys.exit(f"journal has CSUM_V2/V3 (incompat=0x{jsb_feat:x}) — forge would need crc32c")

    has_64bit = bool(jsb_feat & 0x2)

    # ----- Root data block (target of the forged tag) -----
    # Inode 2 is the root directory.
    with open(IMG, "rb") as f:
        f.seek(itab * block_size + (2 - 1) * inode_size)
        root_inode = f.read(inode_size)
    ri_block = root_inode[0x28:0x28 + 60]
    re_ext0  = ri_block[12:24]
    root_data_block = (le16(re_ext0, 6) << 32) | le32(re_ext0, 8)

    with open(IMG, "rb") as f:
        f.seek(root_data_block * block_size)
        on_disk = f.read(block_size)

    new_data = bytes([on_disk[0] ^ 0x42]) + on_disk[1:]
    # Sanity: new_data must NOT begin with JBD_MAGIC (otherwise we'd
    # need ESCAPE flag + zero out first u32). Our XOR target is 0x02 ^ 0x42
    # = 0x40, far from 0xC0.
    if be32(new_data, 0) == JBD_MAGIC:
        sys.exit("forged data accidentally begins with JBD magic")

    # ----- Forge descriptor block -----
    descriptor = bytearray(block_size)
    struct.pack_into(">III", descriptor, 0, JBD_MAGIC, 1, jsb_seq)
    # tag layout (non-CSUM_V3): blocknr(4) | checksum(2) | flags(2) | blocknr_high(4 if 64bit)
    # First tag: no SAME_UUID → UUID (16 bytes) follows the tag.
    LAST_TAG = 0x08
    tag = bytearray()
    tag += struct.pack(">I", root_data_block & 0xFFFFFFFF)
    tag += struct.pack(">H", 0)             # checksum: zero (no CSUM_V2)
    tag += struct.pack(">H", LAST_TAG)      # flags: only LAST_TAG set
    if has_64bit:
        tag += struct.pack(">I", (root_data_block >> 32) & 0xFFFFFFFF)
    tag += jsb_uuid                          # first tag carries UUID
    descriptor[12:12 + len(tag)] = tag

    # ----- Forge commit block -----
    commit = bytearray(block_size)
    struct.pack_into(">III", commit, 0, JBD_MAGIC, 2, jsb_seq)

    with open(IMG, "r+b") as f:
        f.seek(jblk_to_phys(jsb_first)     * block_size); f.write(descriptor)
        f.seek(jblk_to_phys(jsb_first + 1) * block_size); f.write(new_data)
        f.seek(jblk_to_phys(jsb_first + 2) * block_size); f.write(commit)

        # Update journal SB: s_start = first (start of valid log), s_sequence
        # unchanged (seq of the first-expected commit, which is our tx).
        struct.pack_into(">I", jsb, 0x1C, jsb_first)
        f.seek(journal_phys * block_size); f.write(jsb)

        # Set RECOVER on FS feature_incompat.
        f.seek(1024)
        sb_bytes = bytearray(f.read(1024))
        fi = le32(sb_bytes, 0x60) | EXT4_RECOVER
        struct.pack_into("<I", sb_bytes, 0x60, fi)
        f.seek(1024); f.write(sb_bytes)

        # Mirror to backup superblock at block 8193 if sparse_super isn't on.
        # (For 8 MiB / 1 KiB blocks the FS has 1 block group, so no backup.)

    EXPECT.write_text(
        f"fs_block={root_data_block}\n"
        f"journal_blk={jsb_first + 1}\n"
        f"on_disk_byte0=0x{on_disk[0]:02x}\n"
        f"journal_byte0=0x{new_data[0]:02x}\n"
        f"jsb_first={jsb_first}\n"
        f"jsb_sequence={jsb_seq}\n"
    )
    print(f"{IMG}: 1 forged transaction")
    print(f"  fs_block={root_data_block}, journal_blk={jsb_first + 1}")
    print(f"  on-disk byte0=0x{on_disk[0]:02x}, journaled byte0=0x{new_data[0]:02x}")


if __name__ == "__main__":
    main()
