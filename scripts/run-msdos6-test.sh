#!/usr/bin/env bash
# SYNC: Keep this script in sync with scripts/run-freedos-test.sh and
# scripts/run-msdos4-test.sh.  When run-freedos-test.sh gains a new test,
# port it here or mark it "# SKIP(MSDOS6): <reason>".
#
# Boots a MS-DOS 6.22 HDD image with our TSR + test binaries injected,
# runs the same test suite as the FreeDOS runner, then reads OUT.TXT back
# out of the image.
#
# The source image (tests/msdos6/msdos6-source.img) is created once by
# running: make msdos6-image
#
# Override the source image path with MSDOS622_SRC=<path>.
# Override the FAT partition byte offset with PARTITION_OFFSET=<n>
# (default 32256 = 63-sector MBR gap, which is what MS-DOS 6.22 FDISK uses).
set -euo pipefail

DOS_DIR="build/dos"
MSDOS6_DIR="tests/msdos6"
SOURCE_IMG="${MSDOS622_SRC:-tests/msdos6/msdos6-source.img}"
TEST_IMG="$MSDOS6_DIR/test.img"
EXT4_SRC_IMG="tests/images/disk.img"
EXT4_IMG="$MSDOS6_DIR/test-ext4.img"
FREEDOS_IMG="tests/freedos/FD14LITE.img"
PARTITION_OFFSET="${PARTITION_OFFSET:-32256}"
WAIT_SECONDS="${WAIT_SECONDS:-45}"    # extra time for HIMEM.SYS boot

if ! command -v dosbox-x >/dev/null 2>&1; then
    echo "ERROR: dosbox-x not found." >&2
    exit 1
fi
if [[ ! -f "$SOURCE_IMG" ]]; then
    echo "ERROR: $SOURCE_IMG not found." >&2
    echo "  Run: make msdos6-image" >&2
    exit 1
fi
if [[ ! -f "$EXT4_SRC_IMG" ]]; then
    echo "ERROR: $EXT4_SRC_IMG not found. Run: make fixture-partitioned" >&2
    exit 1
fi
if [[ ! -f "$FREEDOS_IMG" ]]; then
    echo "ERROR: $FREEDOS_IMG not found. Run: make tests/freedos/FD14LITE.img" >&2
    exit 1
fi
for f in ext4.exe ext4chk.exe ext4dir.exe ext4cnt.exe ext4dmp.exe ext4wr.exe; do
    if [[ ! -x "$DOS_DIR/$f" ]]; then
        echo "ERROR: $DOS_DIR/$f missing. Run: make dos-build" >&2
        exit 1
    fi
done

mkdir -p "$MSDOS6_DIR"

# Fresh working copies.
cp "$SOURCE_IMG"   "$TEST_IMG"
cp "$EXT4_SRC_IMG" "$EXT4_IMG"

# Extract FDAPM.EXE from the FreeDOS image for graceful DOSBox-X shutdown.
mcopy -i "${FREEDOS_IMG}@@32256" '::FREEDOS/BIN/FDAPM.COM' "$MSDOS6_DIR/fdapm.com"

# Minimal CONFIG.SYS — only LASTDRIVE is required; the TSR does not need XMS.
# If your image relies on HIMEM.SYS for other programs, add DEVICE=C:\DOS\HIMEM.SYS here.
printf 'LASTDRIVE=Z\r\n' > "$MSDOS6_DIR/config.sys"

cat > "$MSDOS6_DIR/autoexec.bat.tmp" <<'EOF'
@echo off
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
echo === DIR Y:\TARGET.TXT (expect size 2048 -- extended) === >> C:\OUT.TXT
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
REM Uninstall must be last -- after this Y: drive is gone.
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
C:\FDAPM.COM POWEROFF
EOF
awk 'BEGIN{ORS="\r\n"} {print}' "$MSDOS6_DIR/autoexec.bat.tmp" > "$MSDOS6_DIR/autoexec.bat"
rm -f "$MSDOS6_DIR/autoexec.bat.tmp"

# Inject everything into the C: partition of the test image.
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4.exe"    ::EXT4.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4chk.exe" ::EXT4CHK.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4dir.exe" ::EXT4DIR.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4cnt.exe" ::EXT4CNT.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4dmp.exe" ::EXT4DMP.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4wr.exe"  ::EXT4WR.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$MSDOS6_DIR/fdapm.com"    ::FDAPM.COM
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$MSDOS6_DIR/config.sys"   ::CONFIG.SYS
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$MSDOS6_DIR/autoexec.bat" ::AUTOEXEC.BAT

LOG="$MSDOS6_DIR/dosbox.log"
rm -f "$LOG"

dosbox-x -fastlaunch -nopromptfolder -exit \
    -c "imgmount 2 $(pwd)/$TEST_IMG -fs none -t hdd" \
    -c "imgmount 3 $(pwd)/$EXT4_IMG -fs none -t hdd" \
    -c "boot c:" \
    >"$LOG" 2>&1 &
BGPID=$!

for i in $(seq 1 "$WAIT_SECONDS"); do
    sleep 1
    if ! kill -0 "$BGPID" 2>/dev/null; then
        echo "DOSBox-X exited after ${i}s"
        break
    fi
done
if kill -0 "$BGPID" 2>/dev/null; then
    kill -9 "$BGPID" 2>/dev/null || true
    sleep 1
    echo "DOSBox-X still running after ${WAIT_SECONDS}s; killed"
fi
wait 2>/dev/null || true

echo
echo "===== OUT.TXT (from MS-DOS 6.22) ====="
OUT=$(mtype -i "$TEST_IMG@@$PARTITION_OFFSET" ::OUT.TXT 2>/dev/null)
if [[ -z "$OUT" ]]; then
    echo "(no OUT.TXT in image)" >&2
    exit 1
fi
echo "$OUT"

# Assertions — mirrors run-freedos-test.sh exactly.
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
if ! grep -q "Hello, ext4-dos!" <<<"$(grep -F -A4 'TYPE Y:\NEWCOPY.TXT' <<<"$OUT")"; then
    echo "FAIL: Y:\\NEWCOPY.TXT doesn't contain expected content (REM_CREATE/REM_WRITE)" >&2
    fail=1
fi
if ! grep -F -A8 'DIR Y:\NEWBIG.TXT' <<<"$OUT" | grep -qE "NEWBIG[[:space:]]+TXT[[:space:]]+2[,]?048"; then
    echo "FAIL: Y:\\NEWBIG.TXT not 2048 bytes after multi-block COPY" >&2
    fail=1
fi
if ! grep -F -A8 'DIR Y:\RENAMED.TXT' <<<"$OUT" | grep -qE "RENAMED[[:space:]]+TXT[[:space:]]+2[,]?048"; then
    echo "FAIL: Y:\\RENAMED.TXT not 2048 bytes after REN (RENAME)" >&2
    fail=1
fi
if ! grep -F -A8 'DIR Y:\NEWDIR' <<<"$OUT" | grep -qE "NEWDIR[[:space:]]+<DIR>"; then
    echo "FAIL: Y:\\NEWDIR not visible as DIR after MD" >&2
    fail=1
fi
if grep -F -A12 'DIR Y: (NEWDIR must be gone)' <<<"$OUT" | grep -qE "NEWDIR[[:space:]]+<DIR>"; then
    echo "FAIL: Y:\\NEWDIR still visible after RD" >&2
    fail=1
fi
INIT_FREE=$(grep -oE '[0-9,]+ bytes free' <<<"$OUT" | head -1 | tr -d ',')
FINAL_FREE=$(grep -oE '[0-9,]+ bytes free' <<<"$OUT" | tail -1 | tr -d ',')
EXPECTED_FINAL=$(( INIT_FREE - 3072 ))
if [[ -z "$INIT_FREE" || "$FINAL_FREE" -ne "$EXPECTED_FINAL" ]]; then
    echo "FAIL: 'bytes free' wrong (expected ${EXPECTED_FINAL}, got ${FINAL_FREE}) — write may not be consuming fs blocks" >&2
    fail=1
fi
if ! grep -qE "verify:.*-> OK" <<<"$OUT"; then
    echo "FAIL: g_fs.sb integrity canary tripped — see 'verify:' lines above" >&2
    fail=1
fi
if grep -F -A12 'DIR Y: after DEL' <<<"$OUT" | grep -qE "NEWCOPY[[:space:]]+TXT"; then
    echo "FAIL: Y:\\NEWCOPY.TXT still visible after DEL" >&2
    fail=1
fi
if ! grep -F -A12 'DIR Y: after DEL' <<<"$OUT" | grep -q "HELLO"; then
    echo "FAIL: Y:\\HELLO.TXT missing from DIR Y: after DEL (should survive)" >&2
    fail=1
fi
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
if ! grep -F -A8 'DIR Y:\TARGET.TXT (expect size 2048' <<<"$OUT" | grep -qE "TARGET[[:space:]]+TXT[[:space:]]+2[,]?048"; then
    echo "FAIL: DIR Y:\\TARGET.TXT after extend didn't show size 2048" >&2
    fail=1
fi
WILD_BLOCK=$(grep -F -A12 'DIR Y:\*.TXT (wildcard)' <<<"$OUT")
if ! grep -q "HELLO" <<<"$WILD_BLOCK"; then
    echo "FAIL: DIR Y:\\*.TXT didn't list HELLO.TXT" >&2
    fail=1
fi
if grep -q "SUBDIR" <<<"$WILD_BLOCK"; then
    echo "FAIL: DIR Y:\\*.TXT incorrectly listed SUBDIR (no .TXT extension)" >&2
    fail=1
fi
if ! grep -q "ext4-dos uninstalled" <<<"$OUT"; then
    echo "FAIL: ext4 -u didn't report uninstalled" >&2
    fail=1
fi
if ! grep -A4 "Re-check" <<<"$OUT" | grep -q "TSR not detected"; then
    echo "FAIL: TSR still detected after ext4 -u" >&2
    fail=1
fi

E2FSCK="$(command -v e2fsck || true)"
for c in /opt/homebrew/opt/e2fsprogs/sbin/e2fsck \
         /usr/local/opt/e2fsprogs/sbin/e2fsck; do
    [[ -x "$c" ]] && E2FSCK="$c" && break
done
if [[ -n "$E2FSCK" ]]; then
    PART_IMG="$MSDOS6_DIR/post-write-part.img"
    dd if="$EXT4_IMG" of="$PART_IMG" bs=512 skip=2048 status=none
    if "$E2FSCK" -fn "$PART_IMG" >"$MSDOS6_DIR/e2fsck.out" 2>&1; then
        rm -f "$PART_IMG" "$MSDOS6_DIR/e2fsck.out"
    else
        E2RC=$?
        echo "FAIL: e2fsck on post-write partition reported errors (rc=$E2RC):" >&2
        cat "$MSDOS6_DIR/e2fsck.out" >&2
        rm -f "$PART_IMG" "$MSDOS6_DIR/e2fsck.out"
        fail=1
    fi
else
    echo "WARN: e2fsck not found — skipping post-write integrity check" >&2
fi

exit $fail
