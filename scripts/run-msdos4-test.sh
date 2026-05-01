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
# MS-DOS 4 status: install + DIR Y: working, TYPE Y: still blocked.
# ============================================================================
# What works:
#   - TSR installs via CONFIG.SYS INSTALL=A:\TSR.EXE.
#   - DIR Y: lists ext4 entries with timestamps and free-space (uses
#     IFS_SEQ_SEARCH_FIRST = INT 2Fh AL=0x19, the MS-DOS-4-specific variant
#     of FindFirst).
#   - Boot proceeds cleanly to A> prompt.
#
# What doesn't:
#   - TYPE Y:\file fails with "File not found". MS-DOS 4 routes the open
#     through INT 2Fh AL=0x2E (Extended Open) instead of the AL=0x16 path
#     FreeDOS uses. We currently alias AL=0x2E to the AL=0x16 handler but
#     something about the SDA/SFT contract for ExtOpen differs and the
#     handler reports failure. See tools/tsr.c near case 0x2E.
#
# Two big lessons learned during the investigation, documented in tools/tsr.c
# but worth repeating here so anyone re-reading this script doesn't re-walk
# the rabbit holes:
#   1. INSTALL=A:\IFSFUNC.EXE is NOT needed. IFSFUNC is for IBM's separate
#      IFS subsystem (DEVICE=foo.IFS drivers). Without IFS drivers loaded
#      it prints "Invalid configuration" (UTIL_ERR_4 in IFSFUNC.SKL) and
#      leaves the kernel in a half-init state — that was the cause of the
#      "Bad or missing Command Interpreter" we chased for ages.
#   2. CDS flags must be `curdir_isnet | curdir_inuse` = 0xC000, NOT just
#      0x8000. FreeDOS works with isnet alone, MS-DOS 4 silently refuses
#      to dispatch redirector calls without inuse set. See
#      references/msdos4/v4.0/src/INC/CURDIR.INC and IFSSESS.ASM.
#   3. The Qualify Remote File Name handler (INT 2Fh AX=1123h) MUST only
#      claim Y:-prefixed paths AND copy the canonical name into ES:DI.
#      Returning CF=0 unconditionally with empty ES:DI worked under
#      FreeDOS (lenient) but bricked MS-DOS 4's COMMAND.COM load.
#
# References cloned at references/msdos4/ (Microsoft v4.0 source, MIT).
# ============================================================================
cat > "$MSDOS4_DIR/config.sys.tmp" <<'EOF'
LASTDRIVE=Z
INSTALL=A:\TSR.EXE -q -t 0x81
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
