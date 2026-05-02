#!/usr/bin/env bash
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
EXT4_IMG="tests/images/disk.img"
PARTITION_OFFSET=32256
WAIT_SECONDS="${WAIT_SECONDS:-30}"

if [[ ! -f "$EXT4_IMG" ]]; then
    echo "ERROR: $EXT4_IMG not found. Run: make fixture-partitioned" >&2
    exit 1
fi

if [[ ! -f "$SOURCE_IMG" ]]; then
    echo "ERROR: $SOURCE_IMG not found." >&2
    echo "  Download:  curl -L -o $SOURCE_IMG.zip 'https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.4/FD14-LiteUSB.zip'" >&2
    echo "  Extract:   unzip -d $FREEDOS_DIR $SOURCE_IMG.zip FD14LITE.img" >&2
    exit 1
fi

for f in ext4.exe ext4chk.exe ext4dir.exe ext4cnt.exe ext4dmp.exe; do
    if [[ ! -x "$DOS_DIR/$f" ]]; then
        echo "ERROR: $DOS_DIR/$f missing. Run: make dos-build" >&2
        exit 1
    fi
done

# Fresh copy of the FreeDOS image — never touch the source.
cp "$SOURCE_IMG" "$TEST_IMG"

# Custom FDAUTO.BAT: skip the FreeDOS installer chain and run our TSR test
# sequence with output captured to C:\OUT.TXT.
cat > "$FREEDOS_DIR/fdauto-test.bat" <<'EOF'
@echo off
SET PATH=C:\FREEDOS\BIN
echo === LOAD TSR (drive 0x81 = ext4 disk) === > C:\OUT.TXT
C:\EXT4.EXE 0x81 >> C:\OUT.TXT
echo === AFTER TSR === >> C:\OUT.TXT
C:\EXT4CHK.EXE >> C:\OUT.TXT
echo === FindFirst Y: (raw INT 21h) === >> C:\OUT.TXT
C:\EXT4DIR.EXE >> C:\OUT.TXT
echo === DIR Y: === >> C:\OUT.TXT
DIR Y: >> C:\OUT.TXT
echo === TYPE Y:\HELLO.TXT === >> C:\OUT.TXT
TYPE Y:\HELLO.TXT >> C:\OUT.TXT
echo === Multi-file: COPY HELLO+NESTED to BOTH.TXT === >> C:\OUT.TXT
COPY /B Y:\HELLO.TXT+Y:\SUBDIR\NESTED.TXT C:\BOTH.TXT >> C:\OUT.TXT
echo === TYPE C:\BOTH.TXT (concatenation result) === >> C:\OUT.TXT
TYPE C:\BOTH.TXT >> C:\OUT.TXT
echo === Read-only enforcement: attempts must FAIL === >> C:\OUT.TXT
echo --- DEL Y:\HELLO.TXT --- >> C:\OUT.TXT
DEL Y:\HELLO.TXT >> C:\OUT.TXT
echo --- COPY C:\BOTH.TXT Y:\NEW.TXT --- >> C:\OUT.TXT
COPY C:\BOTH.TXT Y:\NEW.TXT >> C:\OUT.TXT
echo --- MD Y:\NEWDIR --- >> C:\OUT.TXT
MD Y:\NEWDIR >> C:\OUT.TXT
echo --- (HELLO.TXT must still be there) --- >> C:\OUT.TXT
DIR Y:\HELLO.TXT >> C:\OUT.TXT
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
echo === DONE === >> C:\OUT.TXT
fdapm poweroff
EOF

# Inject everything.
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4.exe"    ::EXT4.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4chk.exe" ::EXT4CHK.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4dir.exe" ::EXT4DIR.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4cnt.exe" ::EXT4CNT.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/ext4dmp.exe" ::EXT4DMP.EXE
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
if ! grep -qE "56[,]?346[,]?624 bytes free" <<<"$OUT"; then
    echo "FAIL: 'bytes free' wrong (expected 56,346,624) — kernel write may be hitting g_safe_*" >&2
    fail=1
fi
if ! grep -qE "verify:.*-> OK" <<<"$OUT"; then
    echo "FAIL: g_fs.sb integrity canary tripped — see 'verify:' lines above" >&2
    fail=1
fi
# Read-only enforcement: HELLO.TXT must survive the DEL attempt.
if ! grep -A2 "HELLO.TXT must still be there" <<<"$OUT" | grep -q "HELLO"; then
    echo "FAIL: read-only enforcement may have allowed DEL Y:\\HELLO.TXT" >&2
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
exit $fail
