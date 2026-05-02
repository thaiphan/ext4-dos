#!/usr/bin/env bash
# SYNC: Keep this script in sync with scripts/run-msdos4-test.sh.
# When adding a new test here, port it there too.  Known MS-DOS 4 skips
# are marked with "# SKIP(MSDOS4):" in that script.
#
# Boots a fresh copy of the FreeDOS LiteUSB image with our TSR + test
# binaries injected, runs the tests via FDAUTO.BAT, then reads the
# captured OUT.TXT back out of the image.
#
# DOSBox-X doesn't exit on fdapm poweroff so we kill it after a timeout —
# DOS-level writes flush through to the image file before then, so the
# OUT.TXT mread afterward is reliable.
set -euo pipefail

DOS_DIR="build/dos"
FREEDOS_DIR="tests/freedos"
SOURCE_IMG="$FREEDOS_DIR/FD14LITE.img"
TEST_IMG="$FREEDOS_DIR/test.img"
EXT4_SRC_IMG="tests/images/disk.img"
EXT4_IMG="$FREEDOS_DIR/test-ext4.img"  # working copy — TSR writes mutate this, not the source
PARTITION_OFFSET=32256
WAIT_SECONDS="${WAIT_SECONDS:-30}"

if [[ ! -f "$EXT4_SRC_IMG" ]]; then
    echo "ERROR: $EXT4_SRC_IMG not found. Run: make fixture-partitioned" >&2
    exit 1
fi


for f in ext4.exe ext4chk.exe ext4dir.exe ext4cnt.exe ext4dmp.exe ext4wr.exe; do
    if [[ ! -x "$DOS_DIR/$f" ]]; then
        echo "ERROR: $DOS_DIR/$f missing. Run: make dos-build" >&2
        exit 1
    fi
done

# Fresh working copies. The ext4 image gets mutated by the TSR's
# REM_WRITE path now that writes go through int13; we don't want those
# persisting into the source fixture.
cp "$SOURCE_IMG"      "$TEST_IMG"
cp "$EXT4_SRC_IMG"    "$EXT4_IMG"

# Custom FDAUTO.BAT: skip the FreeDOS installer chain and run our TSR test
# sequence with output captured to C:\OUT.TXT.
cat > "$FREEDOS_DIR/fdauto-test.bat" <<'EOF'
@echo off
SET PATH=C:\FREEDOS\BIN
echo === LOAD TSR (drive 0x81 = ext4 disk, pin to Y:) === > C:\OUT.TXT
C:\EXT4.EXE 0x81 Y: >> C:\OUT.TXT
echo === AFTER TSR === >> C:\OUT.TXT
C:\EXT4CHK.EXE >> C:\OUT.TXT
echo === FindFirst Y: (raw INT 21h) === >> C:\OUT.TXT
C:\EXT4DIR.EXE >> C:\OUT.TXT
echo === DIR Y: === >> C:\OUT.TXT
DIR Y: >> C:\OUT.TXT
echo === DIR Y:\*.TXT (wildcard) === >> C:\OUT.TXT
DIR Y:\*.TXT >> C:\OUT.TXT
echo === TYPE Y:\HELLO.TXT === >> C:\OUT.TXT
TYPE Y:\HELLO.TXT >> C:\OUT.TXT
echo === Multi-file: COPY HELLO+NESTED to BOTH.TXT === >> C:\OUT.TXT
COPY /B Y:\HELLO.TXT+Y:\SUBDIR\NESTED.TXT C:\BOTH.TXT >> C:\OUT.TXT
echo === TYPE C:\BOTH.TXT (concatenation result) === >> C:\OUT.TXT
TYPE C:\BOTH.TXT >> C:\OUT.TXT
echo === Write test: Y:\TARGET.TXT before === >> C:\OUT.TXT
TYPE Y:\TARGET.TXT >> C:\OUT.TXT
echo --- run ext4wr (in-place B + extend C) --- >> C:\OUT.TXT
C:\EXT4WR.EXE >> C:\OUT.TXT
echo === Y:\TARGET.TXT after write (expect 'B'*1024 + 'C'*1024) === >> C:\OUT.TXT
TYPE Y:\TARGET.TXT >> C:\OUT.TXT
echo === DIR Y:\TARGET.TXT (expect size 2048 — extended) === >> C:\OUT.TXT
DIR Y:\TARGET.TXT >> C:\OUT.TXT
echo === Create file: COPY C:\BOTH.TXT Y:\NEWCOPY.TXT === >> C:\OUT.TXT
COPY C:\BOTH.TXT Y:\NEWCOPY.TXT >> C:\OUT.TXT
echo === TYPE Y:\NEWCOPY.TXT (should match BOTH.TXT contents) === >> C:\OUT.TXT
TYPE Y:\NEWCOPY.TXT >> C:\OUT.TXT
echo === Multi-block copy: COPY Y:\TARGET.TXT Y:\NEWBIG.TXT === >> C:\OUT.TXT
COPY Y:\TARGET.TXT Y:\NEWBIG.TXT >> C:\OUT.TXT
echo === DIR Y:\NEWBIG.TXT (expect size 2048) === >> C:\OUT.TXT
DIR Y:\NEWBIG.TXT >> C:\OUT.TXT
echo === RENAME: REN Y:\NEWBIG.TXT RENAMED.TXT === >> C:\OUT.TXT
REN Y:\NEWBIG.TXT RENAMED.TXT >> C:\OUT.TXT
echo === DIR Y:\RENAMED.TXT (must exist, same size) === >> C:\OUT.TXT
DIR Y:\RENAMED.TXT >> C:\OUT.TXT
echo === Make directory: MD Y:\NEWDIR === >> C:\OUT.TXT
MD Y:\NEWDIR >> C:\OUT.TXT
echo === DIR Y:\NEWDIR (must exist) === >> C:\OUT.TXT
DIR Y:\NEWDIR >> C:\OUT.TXT
echo === Remove directory: RD Y:\NEWDIR === >> C:\OUT.TXT
RD Y:\NEWDIR >> C:\OUT.TXT
echo === DIR Y: (NEWDIR must be gone) === >> C:\OUT.TXT
DIR Y: >> C:\OUT.TXT
echo === DEL Y:\NEWCOPY.TXT (remove created file) === >> C:\OUT.TXT
DEL Y:\NEWCOPY.TXT >> C:\OUT.TXT
echo === DIR Y: after DEL (NEWCOPY must be gone, HELLO still there) === >> C:\OUT.TXT
DIR Y: >> C:\OUT.TXT
echo === TYPE Y:\VERY~876.TXT (8.3 alias roundtrip) === >> C:\OUT.TXT
TYPE Y:\VERY~876.TXT >> C:\OUT.TXT
echo === TYPE Y:\VERY~EB7.TXT (8.3 alias roundtrip) === >> C:\OUT.TXT
TYPE Y:\VERY~EB7.TXT >> C:\OUT.TXT
echo === Verify g_fs.sb integrity (canary) === >> C:\OUT.TXT
C:\EXT4CHK.EXE /V >> C:\OUT.TXT
echo === Subfunction call counts === >> C:\OUT.TXT
C:\EXT4CNT.EXE >> C:\OUT.TXT
echo === FindFirst capture dump === >> C:\OUT.TXT
C:\EXT4DMP.EXE >> C:\OUT.TXT
REM Uninstall must be last — after this Y: drive is gone.
echo === Uninstall: EXT4 -U === >> C:\OUT.TXT
C:\EXT4.EXE -u >> C:\OUT.TXT
echo --- Re-check (should report not-installed) --- >> C:\OUT.TXT
C:\EXT4CHK.EXE >> C:\OUT.TXT
echo === AUTO-DETECT (no args) === >> C:\OUT.TXT
C:\EXT4.EXE >> C:\OUT.TXT
echo --- Re-check (auto-detect should land on D: = first free slot) --- >> C:\OUT.TXT
C:\EXT4CHK.EXE >> C:\OUT.TXT
DIR D: >> C:\OUT.TXT
C:\EXT4.EXE -u >> C:\OUT.TXT
echo === DONE === >> C:\OUT.TXT
fdapm poweroff
EOF

# Inject everything.
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4.exe"    ::EXT4.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4chk.exe" ::EXT4CHK.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4dir.exe" ::EXT4DIR.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4cnt.exe" ::EXT4CNT.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4dmp.exe" ::EXT4DMP.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4wr.exe"  ::EXT4WR.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$FREEDOS_DIR/fdauto-test.bat" ::FDAUTO.BAT

# Boot in DOSBox-X, then kill after timeout (poweroff doesn't exit DOSBox-X).
LOG="$FREEDOS_DIR/test.log"
rm -f "$LOG"

dosbox-x -fastlaunch -nopromptfolder -exit \
    -c "imgmount 2 $(pwd)/$TEST_IMG -fs none -t hdd" \
    -c "imgmount 3 $(pwd)/$EXT4_IMG -fs none -t hdd" \
    -c "boot c:" \
    >"$LOG" 2>&1 &
BGPID=$!

# Wait until DOSBox-X exits OR our timeout elapses.
for i in $(seq 1 "$WAIT_SECONDS"); do
    sleep 1
    if ! kill -0 "$BGPID" 2>/dev/null; then
        echo "DOSBox-X exited after ${i}s"
        break
    fi
done
if kill -0 "$BGPID" 2>/dev/null; then
    kill "$BGPID" 2>/dev/null || true
    sleep 1
    echo "DOSBox-X still running after ${WAIT_SECONDS}s; killed"
fi
wait 2>/dev/null || true

# Pull the captured output back out.
echo
echo "===== OUT.TXT (from FreeDOS) ====="
OUT=$(mtype -i "$TEST_IMG@@$PARTITION_OFFSET" ::OUT.TXT 2>/dev/null)
if [[ -z "$OUT" ]]; then
    echo "(no OUT.TXT in image)" >&2
    exit 1
fi
echo "$OUT"

# Smoke-test assertions. Match the MS-DOS 4 runner's checks.
fail=0
if ! grep -q "Hello, ext4-dos!" <<<"$OUT"; then
    echo "FAIL: TYPE Y:\\HELLO.TXT didn't return file content" >&2
    fail=1
fi
if ! grep -q "long-named file ONE" <<<"$OUT"; then
    echo "FAIL: TYPE Y:\\VERY~876.TXT (8.3 alias) didn't return file content" >&2
    fail=1
fi
if ! grep -q "long-named file TWO" <<<"$OUT"; then
    echo "FAIL: TYPE Y:\\VERY~EB7.TXT (8.3 alias) didn't return file content" >&2
    fail=1
fi
# COPY C:\BOTH.TXT Y:\NEWCOPY.TXT must produce a populated file matching
# BOTH.TXT's contents. Exercises REM_CREATE + the CX=0 pre-extend
# convention + partial-in-place data write.
if ! grep -q "Hello, ext4-dos!" <<<"$(grep -F -A4 'TYPE Y:\NEWCOPY.TXT' <<<"$OUT")"; then
    echo "FAIL: Y:\\NEWCOPY.TXT doesn't contain expected content (REM_CREATE/REM_WRITE)" >&2
    fail=1
fi
# COPY Y:\TARGET.TXT Y:\NEWBIG.TXT exercises multi-block CX=0 pre-extend
# (TARGET.TXT is 2048 bytes = 2 blocks).
if ! grep -F -A8 'DIR Y:\NEWBIG.TXT' <<<"$OUT" | grep -qE "NEWBIG[[:space:]]+TXT[[:space:]]+2[,]?048"; then
    echo "FAIL: Y:\\NEWBIG.TXT not 2048 bytes after multi-block COPY" >&2
    fail=1
fi
# RENAME: NEWBIG.TXT -> RENAMED.TXT must show up at the same size.
if ! grep -F -A8 'DIR Y:\RENAMED.TXT' <<<"$OUT" | grep -qE "RENAMED[[:space:]]+TXT[[:space:]]+2[,]?048"; then
    echo "FAIL: Y:\\RENAMED.TXT not 2048 bytes after REN (RENAME)" >&2
    fail=1
fi
# MD Y:\NEWDIR must produce a visible directory.
if ! grep -F -A8 'DIR Y:\NEWDIR' <<<"$OUT" | grep -qE "NEWDIR[[:space:]]+<DIR>"; then
    echo "FAIL: Y:\\NEWDIR not visible as DIR after MD" >&2
    fail=1
fi
# RD Y:\NEWDIR — must be absent from the subsequent DIR Y: listing.
if grep -F -A12 'DIR Y: (NEWDIR must be gone)' <<<"$OUT" | grep -qE "NEWDIR[[:space:]]+<DIR>"; then
    echo "FAIL: Y:\\NEWDIR still visible after RD" >&2
    fail=1
fi
if ! grep -qE "56[,]?345[,]?600 bytes free" <<<"$OUT"; then
    echo "FAIL: 'bytes free' wrong (expected 56,345,600 with /target.txt 1024B) — kernel write may be hitting g_safe_*" >&2
    fail=1
fi
if ! grep -qE "verify:.*-> OK" <<<"$OUT"; then
    echo "FAIL: g_fs.sb integrity canary tripped — see 'verify:' lines above" >&2
    fail=1
fi
# DEL: NEWCOPY.TXT must be absent after DEL, HELLO.TXT must still be present.
if grep -F -A12 'DIR Y: after DEL' <<<"$OUT" | grep -qE "NEWCOPY[[:space:]]+TXT"; then
    echo "FAIL: Y:\\NEWCOPY.TXT still visible after DEL" >&2
    fail=1
fi
if ! grep -F -A12 'DIR Y: after DEL' <<<"$OUT" | grep -q "HELLO"; then
    echo "FAIL: Y:\\HELLO.TXT missing from DIR Y: after DEL (should survive)" >&2
    fail=1
fi
# Write test: ext4wr reports both writes, and the post-write TYPE shows
# 'B' followed by 'C' (no 'A' left). DOS TYPE doesn't emit a trailing
# newline so the next echo line concatenates; pattern-match for ≥100
# consecutive each of 'B' and 'C', and no 'A'.
if ! grep -q "In-place wrote 1024 bytes" <<<"$OUT"; then
    echo "FAIL: ext4wr didn't report in-place 1024 bytes written" >&2
    fail=1
fi
if ! grep -q "Extend wrote 1024 bytes" <<<"$OUT"; then
    echo "FAIL: ext4wr didn't report extend 1024 bytes written" >&2
    fail=1
fi
WRITE_AFTER=$(grep -F -A1 'Y:\TARGET.TXT after write (expect' <<<"$OUT" | tail -1)
if ! grep -qE 'B{100}' <<<"$WRITE_AFTER"; then
    echo "FAIL: TARGET.TXT after write missing 100+ consecutive 'B' bytes" >&2
    fail=1
fi
if ! grep -qE 'C{100}' <<<"$WRITE_AFTER"; then
    echo "FAIL: TARGET.TXT after write missing 100+ consecutive 'C' bytes (extend not applied)" >&2
    fail=1
fi
if grep -qE 'A{100}' <<<"$WRITE_AFTER"; then
    echo "FAIL: TARGET.TXT after write still has 100+ consecutive 'A' bytes" >&2
    fail=1
fi
# DIR after extend should show 2048 bytes (was 1024). Allow comma or
# no comma for the size formatting.
if ! grep -F -A8 'DIR Y:\TARGET.TXT (expect size 2048' <<<"$OUT" | grep -qE "TARGET[[:space:]]+TXT[[:space:]]+2[,]?048"; then
    echo "FAIL: DIR Y:\\TARGET.TXT after extend didn't show size 2048" >&2
    fail=1
fi
# Wildcard: DIR Y:\*.TXT must list HELLO.TXT and skip SUBDIR (no extension).
# -F (fixed-string) avoids escaping the literal \ and * in the header marker.
WILD_BLOCK=$(grep -F -A12 'DIR Y:\*.TXT (wildcard)' <<<"$OUT")
if ! grep -q "HELLO" <<<"$WILD_BLOCK"; then
    echo "FAIL: DIR Y:\\*.TXT didn't list HELLO.TXT" >&2
    fail=1
fi
if grep -q "SUBDIR" <<<"$WILD_BLOCK"; then
    echo "FAIL: DIR Y:\\*.TXT incorrectly listed SUBDIR (no .TXT extension)" >&2
    fail=1
fi
# Uninstall: TSR should report success and the post-uninstall ext4chk
# should report TSR-not-detected.
if ! grep -q "ext4-dos uninstalled" <<<"$OUT"; then
    echo "FAIL: ext4 -u didn't report uninstalled" >&2
    fail=1
fi
if ! grep -A4 "Re-check" <<<"$OUT" | grep -q "TSR not detected"; then
    echo "FAIL: TSR still detected after ext4 -u" >&2
    fail=1
fi

# Regression net: the TSR's REM_WRITE path must produce an e2fsck-clean
# partition. An inode-checksum-mismatch bug (stack-local crc32c buffers
# under SS!=DS) was invisible to DIR/TYPE assertions — only e2fsck
# flagged it. With metadata_csum on the partitioned fixture, this catches
# any future regression in ext4_inode_recompute_csum and friends.
E2FSCK="$(command -v e2fsck || true)"
for c in /opt/homebrew/opt/e2fsprogs/sbin/e2fsck \
         /usr/local/opt/e2fsprogs/sbin/e2fsck; do
    [[ -x "$c" ]] && E2FSCK="$c" && break
done
if [[ -n "$E2FSCK" ]]; then
    PART_IMG="$FREEDOS_DIR/post-write-part.img"
    dd if="$EXT4_IMG" of="$PART_IMG" bs=512 skip=2048 status=none
    # `set -e` would bail the whole script on e2fsck's non-zero exit;
    # the if-else branch shape captures the rc instead of triggering it.
    if "$E2FSCK" -fn "$PART_IMG" >"$FREEDOS_DIR/e2fsck.out" 2>&1; then
        rm -f "$PART_IMG" "$FREEDOS_DIR/e2fsck.out"
    else
        E2RC=$?
        echo "FAIL: e2fsck on post-write partition reported errors (rc=$E2RC):" >&2
        cat "$FREEDOS_DIR/e2fsck.out" >&2
        rm -f "$PART_IMG" "$FREEDOS_DIR/e2fsck.out"
        fail=1
    fi
else
    echo "WARN: e2fsck not found — skipping post-write integrity check" >&2
fi

exit $fail
