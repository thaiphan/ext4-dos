#!/usr/bin/env bash
# Boot Microsoft's open-source MS-DOS 4.0 (April 2024) bootable floppy in
# DOSBox-X with our TSR + ext4 test fixture attached. Exercises the same
# DIR Y: / TYPE Y:\HELLO.TXT pipeline that works under FreeDOS.
#
# MS-DOS 4 needs IFSFUNC.EXE loaded for INT 2Fh AH=11 redirector dispatch
# to work. The bootable image already ships IFSFUNC.EXE.
set -euo pipefail

DOS_DIR="build/dos"
MSDOS4_DIR="tests/msdos4"
SOURCE_IMG="${MSDOS4_SRC:-tests/msdos4/msdos4-source.img}"
TEST_IMG="$MSDOS4_DIR/test.img"
EXT4_IMG="tests/images/disk.img"
WAIT_SECONDS="${WAIT_SECONDS:-30}"

if ! command -v dosbox-x >/dev/null 2>&1; then
    echo "ERROR: dosbox-x not found." >&2
    exit 1
fi
if [[ ! -f "$SOURCE_IMG" ]]; then
    echo "ERROR: $SOURCE_IMG not found." >&2
    echo "  Set MSDOS4_SRC=<path> to override." >&2
    exit 1
fi
if [[ ! -f "$EXT4_IMG" ]]; then
    echo "ERROR: $EXT4_IMG not found. Run: make fixture-partitioned" >&2
    exit 1
fi
for f in tsr.exe tsr_chk.exe tsr_dir.exe tsr_cnt.exe tsr_dmp.exe; do
    if [[ ! -x "$DOS_DIR/$f" ]]; then
        echo "ERROR: $DOS_DIR/$f missing. Run: make dos-build" >&2
        exit 1
    fi
done

mkdir -p "$MSDOS4_DIR"
cp "$SOURCE_IMG" "$TEST_IMG"

# ============================================================================
# MS-DOS 4 status: PARTIAL — install verified, full DIR/TYPE blocked.
# ============================================================================
# What works:
#   - Our TSR installs cleanly via CONFIG.SYS INSTALL= (visible diagnostic
#     output shows LOL/CDS hookup, ext4 mount, redirector flag set on Y:).
#   - The INT 2Fh AH=11h redirector pathway is the SAME mechanism as DOS 3+
#     and FreeDOS use — kernel dispatches through whatever ISR sits at INT 2Fh.
#
# What doesn't work:
#   - After CONFIG.SYS finishes, MS-DOS 4 fails to load COMMAND.COM with
#     "Bad or missing Command Interpreter" (BADCOM at SYSINIT1.ASM:847).
#     This is BADCOM not BADMEM, so the EXEC of COMMAND.COM fails (not a
#     simple memory shortage). Root cause not isolated; we suspect MCB
#     chain layout interaction between our resident block and SYSINIT's
#     COMMAND.COM EXEC. Same failure with keep size 2048, 3072, 4096
#     paragraphs and full MCB-fitted sizes.
#
# Rabbit holes ALREADY EXPLORED — don't repeat without reading first:
#   1. IFSFUNC.EXE: it is NOT needed. IFSFUNC is for IBM's separate IFS
#      subsystem (DEVICE=foo.IFS drivers). Without IFS drivers loaded, it
#      prints UTIL_ERR_4 = "Invalid configuration" (see
#      references/msdos4/v4.0/src/CMD/IFSFUNC/IFSFUNC.SKL line 48 +
#      IFSINIT.ASM line 1321). Loading it created the misleading
#      "Invalid configuration" message we chased for hours.
#   2. SHELL=A:\COMMAND.COM A:\ /P /E:512 — directive parses fine and lets
#      MS-DOS 4 boot WITHOUT our TSR, but doesn't fix the BADCOM after our
#      TSR installs.
#   3. AUTOEXEC.BAT loading the TSR (vs CONFIG.SYS INSTALL=) — separate
#      MS-DOS 4 bug: _dos_keep from a batch context corrupts COMMAND.COM's
#      batch-position tracking and stalls at "Insert disk with batch file".
#      COMMAND /C sub-shell wrapper doesn't help — corruption propagates
#      to the parent.
#   4. Freeing our environment block before _dos_keep — REGRESSED FreeDOS
#      (broke FindFirst). The fix that actually helped was using the MCB-
#      reported allocation size in _dos_keep instead of a hard-coded
#      paragraph count; see tools/tsr.c.
#
# Next step IF revisiting MS-DOS 4: build DOSBox-X with --enable-debug,
# breakpoint SYSINIT1.ASM:847 and backtrace to see whether OPEN, LSEEK, or
# EXEC fails, then inspect MCB chain at the moment of failure.
#
# The redirector itself is proven correct under FreeDOS — the issue here is
# DOS-internals plumbing, not ext4 logic.
# ============================================================================
cat > "$MSDOS4_DIR/config.sys.tmp" <<'EOF'
LASTDRIVE=Z
INSTALL=A:\TSR.EXE -q 0x81
EOF
awk 'BEGIN{ORS="\r\n"} {print}' "$MSDOS4_DIR/config.sys.tmp" > "$MSDOS4_DIR/config.sys"
rm -f "$MSDOS4_DIR/config.sys.tmp"

cat > "$MSDOS4_DIR/autoexec.bat.tmp" <<'EOF'
@echo off
echo === AFTER TSR === > A:\OUT.TXT
A:\TSR_CHK.EXE >> A:\OUT.TXT
echo === FindFirst Y: === >> A:\OUT.TXT
A:\TSR_DIR.EXE >> A:\OUT.TXT
echo === DIR Y: === >> A:\OUT.TXT
DIR Y: >> A:\OUT.TXT
echo === TYPE Y:\HELLO.TXT === >> A:\OUT.TXT
TYPE Y:\HELLO.TXT >> A:\OUT.TXT
echo === Subfunction call counts === >> A:\OUT.TXT
A:\TSR_CNT.EXE >> A:\OUT.TXT
echo === DONE === >> A:\OUT.TXT
EOF
awk 'BEGIN{ORS="\r\n"} {print}' "$MSDOS4_DIR/autoexec.bat.tmp" > "$MSDOS4_DIR/autoexec.bat"
rm -f "$MSDOS4_DIR/autoexec.bat.tmp"

# Floppy is plain FAT12 with no MBR offset; mtools accesses raw image.
mcopy -i "$TEST_IMG" -o "$DOS_DIR/tsr.exe"     ::TSR.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/tsr_chk.exe" ::TSR_CHK.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/tsr_dir.exe" ::TSR_DIR.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/tsr_cnt.exe" ::TSR_CNT.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/tsr_dmp.exe" ::TSR_DMP.EXE
mcopy -i "$TEST_IMG" -o "$MSDOS4_DIR/config.sys"   ::CONFIG.SYS
mcopy -i "$TEST_IMG" -o "$MSDOS4_DIR/autoexec.bat" ::AUTOEXEC.BAT

LOG="$MSDOS4_DIR/dosbox.log"
rm -f "$LOG"

dosbox-x -fastlaunch -nopromptfolder -exit \
    -c "imgmount A: $(pwd)/$TEST_IMG -t floppy" \
    -c "imgmount 3 $(pwd)/$EXT4_IMG -fs none -t hdd" \
    -c "boot A:" \
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
    kill "$BGPID" 2>/dev/null || true
    sleep 1
    echo "DOSBox-X still running after ${WAIT_SECONDS}s; killed"
fi
wait 2>/dev/null || true

echo
echo "===== OUT.TXT (from MS-DOS 4) ====="
mtype -i "$TEST_IMG" ::OUT.TXT 2>/dev/null || echo "(no OUT.TXT in image)"
