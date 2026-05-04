#!/usr/bin/env python3
"""Build a fixture with a dirty journal containing > EXT4_JBD_REPLAY_MAP_MAX
unique fs_blocks — enough to overflow the in-memory replay map, exercising
the writable-mount streaming flush path (JBD_ACTION_FLUSH).

Strategy: forge ONE transaction with N_TAGS distinct tags. Each tag
points at a distinct fs_block well inside the journal's own physical
extent (logical journal block 200..200+N_TAGS), so when the streaming
walker flushes them the writes land on unused log space — no real
ext4 metadata is disturbed and e2fsck stays happy.

Layout in the journal:
   jsb.first       descriptor (N_TAGS tags)
   jsb.first+1..N  data blocks (each filled with a recognizable pattern)
   jsb.first+N+1   commit
"""

import os
import struct
import subprocess
import sys
from pathlib import Path

ROOT       = Path(__file__).resolve().parent.parent
IMG        = ROOT / "tests/images/journal-large.img"
EXPECT     = ROOT / "tests/images/journal-large.expect"
FS_SIZE_MB = 32          # big enough for a ~1 MiB journal
N_TAGS     = 80          # > EXT4_JBD_REPLAY_MAP_MAX (64)

JBD_MAGIC    = 0xC03B3998
EXT4_MAGIC   = 0xEF53
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
    # No metadata_csum → no journal CSUM_V2/V3 → forge skips CRC32C.
    # Force a 1 MiB journal (default would be smaller for 32 MiB FS).
    subprocess.run(
        [mkfs, "-F",
         "-b", "1024",
         "-O", "^metadata_csum,has_journal",
         "-J", "size=1",
         "-N", "64",
         "-L", "ext4-dos-jrnlL",
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
    inode_size       = le16(sb, 0x58)
    feat_incompat    = le32(sb, 0x60)
    bgd_size         = 64 if (feat_incompat & 0x80) else 32

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
    extents = []
    for k in range(n_ext):
        e = i_block[12 + k * 12: 12 + (k + 1) * 12]
        e_log  = le32(e, 0)
        e_len  = le16(e, 4) & 0x7FFF
        e_phys = (le16(e, 6) << 32) | le32(e, 8)
        extents.append((e_log, e_len, e_phys))
    if extents[0][0] != 0:
        sys.exit("journal extent[0] doesn't start at logical 0")
    journal_phys = extents[0][2]

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

    jsb_first  = be32(jsb, 0x14)
    jsb_maxlen = be32(jsb, 0x10)
    jsb_seq    = be32(jsb, 0x18)
    jsb_start  = be32(jsb, 0x1C)
    jsb_feat   = be32(jsb, 0x28)
    jsb_uuid   = bytes(jsb[0x30:0x40])

    if jsb_start != 0:
        sys.exit("journal not clean before forging")
    if jsb_feat & 0x18:
        sys.exit(f"journal has CSUM_V2/V3 (incompat=0x{jsb_feat:x}) — forge would need crc32c")

    has_64bit = bool(jsb_feat & 0x2)

    # We use:
    #   descriptor at jsb.first
    #   data       at jsb.first+1 .. jsb.first+N_TAGS
    #   commit     at jsb.first+N_TAGS+1
    # Tag-target fs_blocks point at logical journal blocks 200..200+N_TAGS,
    # which sit far past the forged transaction in the log — flushing them
    # lands on unused log space.
    last_used = jsb_first + N_TAGS + 1
    target_logical_base = max(200, last_used + 16)
    if target_logical_base + N_TAGS > jsb_maxlen:
        sys.exit(f"journal too small: maxlen={jsb_maxlen}, need ≥ {target_logical_base + N_TAGS}")

    # Tag descriptor budget: header(12) + UUID(16) + N*tag_bytes(12 if 64bit else 8) ≤ blocksize
    tag_bytes = 12 if has_64bit else 8
    budget = block_size - 12 - 16
    if N_TAGS * tag_bytes > budget:
        sys.exit(f"too many tags for descriptor: budget={budget}, need={N_TAGS * tag_bytes}")

    # ----- Forge descriptor block -----
    LAST_TAG    = 0x08
    SAME_UUID   = 0x02

    descriptor = bytearray(block_size)
    struct.pack_into(">III", descriptor, 0, JBD_MAGIC, 1, jsb_seq)

    target_fs_blocks = []
    off = 12
    for i in range(N_TAGS):
        target_logical = target_logical_base + i
        target_phys    = jblk_to_phys(target_logical)
        target_fs_blocks.append(target_phys)

        flags = 0
        if i + 1 == N_TAGS:
            flags |= LAST_TAG
        if i > 0:
            flags |= SAME_UUID

        # tag layout (non-CSUM_V3): blocknr(4) | csum16(2) | flags(2) | blocknr_high(4 if 64bit)
        struct.pack_into(">I", descriptor, off + 0, target_phys & 0xFFFFFFFF)
        struct.pack_into(">H", descriptor, off + 4, 0)              # csum16: zero (no csum)
        struct.pack_into(">H", descriptor, off + 6, flags)
        if has_64bit:
            struct.pack_into(">I", descriptor, off + 8, (target_phys >> 32) & 0xFFFFFFFF)
        off += tag_bytes

        # First tag carries UUID immediately after.
        if i == 0:
            descriptor[off:off + 16] = jsb_uuid
            off += 16

    # ----- Forge commit block -----
    commit = bytearray(block_size)
    struct.pack_into(">III", commit, 0, JBD_MAGIC, 2, jsb_seq)

    # ----- Build per-tag data blocks (recognizable per-index pattern) -----
    # Byte0 of each data block = i (mod 256). Byte1 = "marker" 0x5A. The
    # streaming flush should land these bytes into the target fs_blocks; we
    # verify the first one in the test.
    data_blocks = []
    for i in range(N_TAGS):
        d = bytearray(block_size)
        d[0] = i & 0xFF
        d[1] = 0x5A
        # Avoid accidental JBD_MAGIC at byte 0 of any data block. The
        # magic is 0xC03B3998 (big-endian); byte0 = 0xC0 = 192. With i in
        # [0..79] we're safe.
        if d[0] == 0xC0 and d[1] == 0x3B:
            sys.exit("forged data accidentally begins with JBD magic")
        data_blocks.append(bytes(d))

    # ----- Write everything -----
    with open(IMG, "r+b") as f:
        # Descriptor.
        f.seek(jblk_to_phys(jsb_first) * block_size); f.write(descriptor)
        # Per-tag data blocks.
        for i in range(N_TAGS):
            f.seek(jblk_to_phys(jsb_first + 1 + i) * block_size)
            f.write(data_blocks[i])
        # Commit.
        f.seek(jblk_to_phys(jsb_first + 1 + N_TAGS) * block_size)
        f.write(commit)

        # Update journal SB: s_start = first.
        struct.pack_into(">I", jsb, 0x1C, jsb_first)
        f.seek(journal_phys * block_size); f.write(jsb)

        # Set RECOVER on FS feature_incompat.
        f.seek(1024)
        sb_bytes = bytearray(f.read(1024))
        fi = le32(sb_bytes, 0x60) | EXT4_RECOVER
        struct.pack_into("<I", sb_bytes, 0x60, fi)
        f.seek(1024); f.write(sb_bytes)

    EXPECT.write_text(
        f"n_tags={N_TAGS}\n"
        f"first_target_fs_block={target_fs_blocks[0]}\n"
        f"first_target_byte0=0x00\n"
        f"first_target_byte1=0x5a\n"
        f"jsb_first={jsb_first}\n"
        f"jsb_sequence={jsb_seq}\n"
    )
    print(f"{IMG}: 1 forged transaction with {N_TAGS} tags")
    print(f"  jsb.first={jsb_first}, target fs_blocks=[{target_fs_blocks[0]}..{target_fs_blocks[-1]}]")


if __name__ == "__main__":
    main()
