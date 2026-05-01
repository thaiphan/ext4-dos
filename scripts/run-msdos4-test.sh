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
for f in ext4.exe ext4chk.exe ext4dir.exe ext4cnt.exe ext4dmp.exe; do
    if [[ ! -x "$DOS_DIR/$f" ]]; then
        echo "ERROR: $DOS_DIR/$f missing. Run: make dos-build" >&2
        exit 1
    fi
done

mkdir -p "$MSDOS4_DIR"
cp "$SOURCE_IMG" "$TEST_IMG"

# ============================================================================
# MS-DOS 4 status: full DIR + TYPE working end-to-end.
# ============================================================================
# Lessons learned during the bring-up — read these before changing anything,
# they're the kind of bug that takes hours to find and seconds to undo:
#
#   1. INSTALL=A:\IFSFUNC.EXE is NOT needed. IFSFUNC is for IBM's separate
#      IFS subsystem (DEVICE=foo.IFS drivers). Without IFS drivers loaded
#      it prints "Invalid configuration" (UTIL_ERR_4 in IFSFUNC.SKL) and
#      leaves the kernel in a half-init state — that was the cause of the
#      "Bad or missing Command Interpreter" we chased for ages.
#
#   2. CDS flags must be `curdir_isnet | curdir_inuse` = 0xC000, NOT just
#      0x8000. FreeDOS works with isnet alone, MS-DOS 4 silently refuses
#      to dispatch redirector calls without inuse set. See
#      references/msdos4/v4.0/src/INC/CURDIR.INC and IFSSESS.ASM.
#
#   3. The Qualify Remote File Name handler (INT 2Fh AX=1123h) must only
#      claim Y:-prefixed paths AND copy the canonical name into ES:DI.
#      Returning CF=0 unconditionally with empty ES:DI worked under
#      FreeDOS (lenient) but bricked MS-DOS 4's COMMAND.COM load.
#
#   4. MS-DOS 4 dispatches several redirector subfunctions FreeDOS doesn't:
#        AL=0x19 IFS_SEQ_SEARCH_FIRST (FindFirst — required for DIR Y:)
#        AL=0x2E Extended Open       (issued from AX=6C00h ExtOpen path)
#        AL=0x2D Get/Set XA          (issued from AH=57h Get/Set
#                                     Extended Attributes — TYPE calls
#                                     this immediately after Open to
#                                     read the file's code page tag.
#                                     If we don't claim it, the kernel
#                                     returns "Invalid function" to the
#                                     caller and TYPE bombs.)
#      All three are now wired up in tools/tsr.c.
#
#   5. The unknown-subfunction default arm CHAINS to prev_int2f rather
#      than claiming "file not found". Returning errors for subfunctions
#      we don't implement confused MS-DOS 4 in subtle ways.
#
# References cloned at references/msdos4/ (Microsoft v4.0 source, MIT).
# Useful starting points if you need to revisit:
#   src/DOS/OPEN.ASM, HANDLE.ASM, ISEARCH.ASM — kernel dispatch points
#   src/INC/CURDIR.INC                        — CDS structure + flag bits
#   src/INC/mult.inc                          — INT 2Fh subfunction list
#   src/CMD/IFSFUNC/                          — IFSFUNC source (NOT needed)
# ============================================================================
cat > "$MSDOS4_DIR/config.sys.tmp" <<'EOF'
LASTDRIVE=Z
INSTALL=A:\EXT4.EXE -q 0x81
EOF
awk 'BEGIN{ORS="\r\n"} {print}' "$MSDOS4_DIR/config.sys.tmp" > "$MSDOS4_DIR/config.sys"
rm -f "$MSDOS4_DIR/config.sys.tmp"

cat > "$MSDOS4_DIR/autoexec.bat.tmp" <<'EOF'
@echo off
echo === AFTER TSR === > A:\OUT.TXT
A:\EXT4CHK.EXE >> A:\OUT.TXT
echo === FindFirst Y: === >> A:\OUT.TXT
A:\EXT4DIR.EXE >> A:\OUT.TXT
echo === DIR Y: === >> A:\OUT.TXT
DIR Y: >> A:\OUT.TXT
echo === TYPE Y:\HELLO.TXT === >> A:\OUT.TXT
TYPE Y:\HELLO.TXT >> A:\OUT.TXT
echo === Multi-file: COPY HELLO+NESTED to BOTH.TXT === >> A:\OUT.TXT
COPY /B Y:\HELLO.TXT+Y:\SUBDIR\NESTED.TXT A:\BOTH.TXT >> A:\OUT.TXT
echo === TYPE A:\BOTH.TXT (concatenation result) === >> A:\OUT.TXT
TYPE A:\BOTH.TXT >> A:\OUT.TXT
echo === Subfunction call counts === >> A:\OUT.TXT
A:\EXT4CNT.EXE >> A:\OUT.TXT
echo === TSR diagnostic dump === >> A:\OUT.TXT
A:\EXT4DMP.EXE >> A:\OUT.TXT
echo === DONE === >> A:\OUT.TXT
EOF
awk 'BEGIN{ORS="\r\n"} {print}' "$MSDOS4_DIR/autoexec.bat.tmp" > "$MSDOS4_DIR/autoexec.bat"
rm -f "$MSDOS4_DIR/autoexec.bat.tmp"

# Floppy is plain FAT12 with no MBR offset; mtools accesses raw image.
mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4.exe"    ::EXT4.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4chk.exe" ::EXT4CHK.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4dir.exe" ::EXT4DIR.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4cnt.exe" ::EXT4CNT.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4dmp.exe" ::EXT4DMP.EXE
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
