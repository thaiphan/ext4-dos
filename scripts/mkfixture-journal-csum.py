#!/usr/bin/env python3
"""Build a CSUM_V2 dirty-journal fixture by force.

The macOS e2fsprogs we have available doesn't emit CSUM_V2/V3 in the
journal even with metadata_csum on. So we build a clean FS, then
*patch* the journal superblock to claim CSUM_V2, recompute its
self-checksum, and forge a transaction whose descriptor tail and
commit chksum[0] both carry valid CRC32C values.

The C-side walker should accept the transaction. Bit-flipping any of
the checksums in this image would cause the walker to bail at that
block — that's what verify_meta_block / verify_commit_block are for.
"""

import os
import struct
import subprocess
import sys
from pathlib import Path

ROOT       = Path(__file__).resolve().parent.parent
IMG        = ROOT / "tests/images/journal-csum.img"
EXPECT     = ROOT / "tests/images/journal-csum.expect"
FS_SIZE_MB = 8

JBD_MAGIC  = 0xC03B3998
EXT4_MAGIC = 0xEF53
EXT4_RECOVER = 0x4
JBD_INCOMPAT_CSUM_V2 = 0x8

CRC32C_POLY_REFLECTED = 0x82F63B78

def _build_table():
    tab = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ CRC32C_POLY_REFLECTED if (c & 1) else (c >> 1)
        tab.append(c)
    return tab

_TAB = _build_table()


def crc32c(seed, buf):
    crc = seed
    for b in buf:
        crc = _TAB[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc & 0xFFFFFFFF


def le16(b, o): return struct.unpack_from("<H", b, o)[0]
def le32(b, o): return struct.unpack_from("<I", b, o)[0]
def be32(b, o): return struct.unpack_from(">I", b, o)[0]


MKFS_CANDIDATES = [
    "mkfs.ext4",
    "/opt/homebrew/opt/e2fsprogs/sbin/mkfs.ext4",
    "/usr/local/opt/e2fsprogs/sbin/mkfs.ext4",
    "/sbin/mkfs.ext4",
]


def find_mkfs():
    import shutil
    found = shutil.which("mkfs.ext4")
    if found: return found
    for p in MKFS_CANDIDATES:
        if os.path.exists(p): return p
    sys.exit("mkfs.ext4 not found")


def main():
    IMG.parent.mkdir(parents=True, exist_ok=True)
    if IMG.exists(): IMG.unlink()
    subprocess.run(["truncate", "-s", f"{FS_SIZE_MB}M", str(IMG)], check=True)
    subprocess.run(
        [find_mkfs(), "-F", "-b", "1024",
         "-O", "^metadata_csum,has_journal",
         "-N", "64",
         "-L", "ext4-dos-jcsum",
         str(IMG)],
        check=True,
        stdout=subprocess.DEVNULL,
    )

    with open(IMG, "rb") as f: f.seek(1024); sb = f.read(1024)
    if le16(sb, 0x38) != EXT4_MAGIC: sys.exit("not ext4")

    block_size       = 1024 << le32(sb, 0x18)
    journal_inum     = le32(sb, 0xE0)
    inode_size       = le16(sb, 0x58)
    feat_incompat    = le32(sb, 0x60)
    bgd_size         = 64 if (feat_incompat & 0x80) else 32
    bgd_block        = 1 if block_size > 1024 else 2

    with open(IMG, "rb") as f:
        f.seek(bgd_block * block_size); bgd0 = f.read(bgd_size)
    itab = le32(bgd0, 0x08) | ((le32(bgd0, 0x28) if bgd_size == 64 else 0) << 32)

    with open(IMG, "rb") as f:
        f.seek(itab * block_size + (journal_inum - 1) * inode_size)
        jinode = f.read(inode_size)
    i_block = jinode[0x28:0x28 + 60]
    if le16(i_block, 0) != 0xF30A or le16(i_block, 6) != 0:
        sys.exit("journal extent layout unsupported by script")
    extents = []
    for k in range(le16(i_block, 2)):
        e = i_block[12 + k * 12: 12 + (k + 1) * 12]
        extents.append((le32(e, 0), le16(e, 4) & 0x7FFF,
                        (le16(e, 6) << 32) | le32(e, 8)))
    journal_phys = extents[0][2]

    def jblk_to_phys(jblk):
        for (e_log, e_len, e_phys) in extents:
            if e_log <= jblk < e_log + e_len:
                return e_phys + (jblk - e_log)
        sys.exit(f"jblk {jblk} not covered")

    # ----- Read clean journal SB, patch in CSUM_V2 -----
    with open(IMG, "rb") as f:
        f.seek(journal_phys * block_size); jsb = bytearray(f.read(block_size))
    if be32(jsb, 0) != JBD_MAGIC: sys.exit("bad jbd magic")

    jsb_first = be32(jsb, 0x14)
    jsb_seq   = be32(jsb, 0x18)
    if be32(jsb, 0x1C) != 0:
        sys.exit("journal not clean before forging")
    has_64bit = bool(be32(jsb, 0x28) & 0x2)

    # Force blocktype to V2 (4 — has feature flags) and set CSUM_V2.
    struct.pack_into(">I", jsb, 0x04, 4)  # bhdr.blocktype = JBD_SUPERBLOCK_V2
    new_feat = be32(jsb, 0x28) | JBD_INCOMPAT_CSUM_V2
    struct.pack_into(">I", jsb, 0x28, new_feat)
    # checksum_type at byte 0x50 = JBD_CRC32C_CHKSUM (4)
    jsb[0x50] = 4
    # We may need a UUID — mkfs usually populates 0x30..0x40. Keep as-is.
    jsb_uuid = bytes(jsb[0x30:0x40])

    # Recompute jsb.checksum at offset 0x100 - 4 = 0xFC.
    # Per jbd_sb: it's the last u32 of the 1 KiB header section.
    # crc = crc32c(jsb_with_chksum_field_zeroed).
    struct.pack_into(">I", jsb, 0xFC, 0)
    jsb_crc = crc32c(0xFFFFFFFF, bytes(jsb))
    struct.pack_into(">I", jsb, 0xFC, jsb_crc)

    # ----- Pick target block (root data) and forge new bytes -----
    with open(IMG, "rb") as f:
        f.seek(itab * block_size + (2 - 1) * inode_size)
        root_inode = f.read(inode_size)
    re_ext0 = root_inode[0x28 + 12 : 0x28 + 12 + 12]
    root_data_block = (le16(re_ext0, 6) << 32) | le32(re_ext0, 8)

    with open(IMG, "rb") as f:
        f.seek(root_data_block * block_size); on_disk = f.read(block_size)
    new_data = bytes([on_disk[0] ^ 0x42]) + on_disk[1:]

    # ----- Forge descriptor with CSUM_V2 tail -----
    # CSUM_V2 tag layout: blocknr(4) | per_tag_csum16(2) | flags(2) | blocknr_high(4 if 64bit)
    LAST_TAG = 0x08
    seq_be = struct.pack(">I", jsb_seq)
    per_tag_full = crc32c(0xFFFFFFFF, jsb_uuid)
    per_tag_full = crc32c(per_tag_full, seq_be)
    per_tag_full = crc32c(per_tag_full, new_data)
    per_tag16 = per_tag_full & 0xFFFF

    descriptor = bytearray(block_size)
    struct.pack_into(">III", descriptor, 0, JBD_MAGIC, 1, jsb_seq)
    tag = bytearray()
    tag += struct.pack(">I", root_data_block & 0xFFFFFFFF)
    tag += struct.pack(">H", per_tag16)
    tag += struct.pack(">H", LAST_TAG)
    if has_64bit:
        tag += struct.pack(">I", (root_data_block >> 32) & 0xFFFFFFFF)
    tag += jsb_uuid
    descriptor[12:12 + len(tag)] = tag

    # Tail crc at last 4 bytes — crc32c(uuid + descriptor_with_tail_zeroed).
    desc_view = bytes(descriptor[:-4]) + b"\0\0\0\0"
    tail_crc = crc32c(0xFFFFFFFF, jsb_uuid)
    tail_crc = crc32c(tail_crc, desc_view)
    struct.pack_into(">I", descriptor, block_size - 4, tail_crc)

    # ----- Forge commit with valid chksum[0] -----
    commit = bytearray(block_size)
    struct.pack_into(">III", commit, 0, JBD_MAGIC, 2, jsb_seq)
    commit[12] = 4  # chksum_type = CRC32C
    commit[13] = 4  # chksum_size = 4
    commit_view = bytes(commit[:16]) + b"\0\0\0\0" + bytes(commit[20:])
    commit_crc = crc32c(0xFFFFFFFF, jsb_uuid)
    commit_crc = crc32c(commit_crc, commit_view)
    struct.pack_into(">I", commit, 16, commit_crc)

    # ----- Write everything back -----
    with open(IMG, "r+b") as f:
        f.seek(jblk_to_phys(jsb_first    ) * block_size); f.write(descriptor)
        f.seek(jblk_to_phys(jsb_first + 1) * block_size); f.write(new_data)
        f.seek(jblk_to_phys(jsb_first + 2) * block_size); f.write(commit)

        struct.pack_into(">I", jsb, 0x1C, jsb_first)  # s_start
        # s_sequence is unchanged (already the seq of next-expected commit).
        f.seek(journal_phys * block_size); f.write(jsb)

        f.seek(1024); sb_bytes = bytearray(f.read(1024))
        fi = le32(sb_bytes, 0x60) | EXT4_RECOVER
        struct.pack_into("<I", sb_bytes, 0x60, fi)
        f.seek(1024); f.write(sb_bytes)

    EXPECT.write_text(
        f"fs_block={root_data_block}\n"
        f"journal_blk={jsb_first + 1}\n"
        f"on_disk_byte0=0x{on_disk[0]:02x}\n"
        f"journal_byte0=0x{new_data[0]:02x}\n"
        f"jsb_first={jsb_first}\n"
        f"jsb_sequence={jsb_seq}\n"
        f"csum_v2=1\n"
    )
    print(f"{IMG}: 1 forged transaction with CSUM_V2 checksums")
    print(f"  fs_block={root_data_block} journal_blk={jsb_first + 1}")
    print(f"  on-disk byte0=0x{on_disk[0]:02x} journaled byte0=0x{new_data[0]:02x}")


if __name__ == "__main__":
    main()
