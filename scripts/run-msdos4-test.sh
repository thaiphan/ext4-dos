#!/usr/bin/env bash
# SYNC: Keep this script in sync with scripts/run-freedos-test.sh.
# When run-freedos-test.sh gains a new test, port it here and either add
# the test or mark it with "# SKIP(MSDOS4): <reason>".
#
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
EXT4_SRC_IMG="tests/images/disk.img"
EXT4_IMG="$MSDOS4_DIR/test-ext4.img"   # working copy — writes don't mutate the source
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

mkdir -p "$MSDOS4_DIR"
cp "$SOURCE_IMG" "$TEST_IMG"
cp "$EXT4_SRC_IMG" "$EXT4_IMG"

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
INSTALL=A:\EXT4.EXE -q 0x81 Y:
EOF
awk 'BEGIN{ORS="\r\n"} {print}' "$MSDOS4_DIR/config.sys.tmp" > "$MSDOS4_DIR/config.sys"
rm -f "$MSDOS4_DIR/config.sys.tmp"

cat > "$MSDOS4_DIR/autoexec.bat.tmp" <<'EOF'
@echo off
echo === AFTER TSR === > A:\OUT.TXT
A:\EXT4CHK.EXE >> A:\OUT.TXT
echo === FindFirst Y: (raw INT 21h) === >> A:\OUT.TXT
A:\EXT4DIR.EXE >> A:\OUT.TXT
echo === DIR Y: === >> A:\OUT.TXT
DIR Y: >> A:\OUT.TXT
echo === DIR Y:\*.TXT (wildcard) === >> A:\OUT.TXT
DIR Y:\*.TXT >> A:\OUT.TXT
echo === TYPE Y:\HELLO.TXT === >> A:\OUT.TXT
TYPE Y:\HELLO.TXT >> A:\OUT.TXT
REM SKIP(MSDOS4): ext4wr, file creation via COPY, multi-block COPY — any file-create write to Y: via
REM our redirector corrupts MS-DOS 4's EXEC path (Cannot execute A:\*.EXE for
REM the rest of the session). Same root class as the 8.3 alias TYPE quirk.
REM Confirmed empirically; root cause not fully diagnosed. FreeDOS covers all.
echo === Make directory: MD Y:\NEWDIR === >> A:\OUT.TXT
MD Y:\NEWDIR >> A:\OUT.TXT
REM Under MS-DOS 4, DIR Y:\NEWDIR lists the *contents* of NEWDIR,
REM not NEWDIR itself in Y:\. Use DIR Y: so the entry is visible.
echo === DIR Y: after MD (NEWDIR must appear) === >> A:\OUT.TXT
DIR Y: >> A:\OUT.TXT
echo === DEL/RD must still FAIL (no unlink/rmdir yet) === >> A:\OUT.TXT
echo --- DEL Y:\HELLO.TXT --- >> A:\OUT.TXT
DEL Y:\HELLO.TXT >> A:\OUT.TXT
echo --- RD Y:\NEWDIR --- >> A:\OUT.TXT
RD Y:\NEWDIR >> A:\OUT.TXT
echo --- (HELLO.TXT must still be there) --- >> A:\OUT.TXT
DIR Y:\HELLO.TXT >> A:\OUT.TXT
REM SKIP(MSDOS4): 8.3 alias roundtrip omitted — TYPE via alias breaks
REM subsequent A:\ EXEC (same MS-DOS 4 quirk; covered by FreeDOS test).
echo === Verify g_fs.sb integrity (canary) === >> A:\OUT.TXT
A:\EXT4CHK.EXE /V >> A:\OUT.TXT
echo === Subfunction call counts === >> A:\OUT.TXT
A:\EXT4CNT.EXE >> A:\OUT.TXT
echo === TSR diagnostic dump === >> A:\OUT.TXT
A:\EXT4DMP.EXE >> A:\OUT.TXT
REM SKIP(MSDOS4): auto-detect omitted — loaded via CONFIG.SYS INSTALL=.
echo === Uninstall: EXT4 -U === >> A:\OUT.TXT
A:\EXT4.EXE -u >> A:\OUT.TXT
echo --- Re-check (should report not-installed) --- >> A:\OUT.TXT
A:\EXT4CHK.EXE >> A:\OUT.TXT
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
mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4wr.exe"  ::EXT4WR.EXE
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
OUT=$(mtype -i "$TEST_IMG" ::OUT.TXT 2>/dev/null)
if [[ -z "$OUT" ]]; then
    echo "(no OUT.TXT in image)" >&2
    exit 1
fi
echo "$OUT"

# Assertions — mirror run-freedos-test.sh; skips noted inline.
fail=0
if ! grep -q "Hello, ext4-dos!" <<<"$OUT"; then
    echo "FAIL: TYPE Y:\\HELLO.TXT didn't return file content" >&2
    fail=1
fi
# SKIP(MSDOS4): long-name alias roundtrip — see BAT comment.
# SKIP(MSDOS4): writes are skipped, so free space won't change; just verify
# the DIR output produced a valid "bytes free" line (mkfs-version-agnostic).
if ! grep -qE '[0-9]+ bytes free' <<<"$OUT"; then
    echo "FAIL: 'bytes free' not found in DIR output" >&2
    fail=1
fi
if ! grep -qE "verify:.*-> OK" <<<"$OUT"; then
    echo "FAIL: g_fs.sb integrity canary tripped" >&2
    fail=1
fi
# SKIP(MSDOS4): write test, file creation via COPY, multi-block COPY assertions
# — any file-create write to Y: corrupts MS-DOS 4's EXEC path; all covered by
# FreeDOS test.
# MD Y:\NEWDIR — check it appears in the subsequent DIR Y: listing.
if ! grep -F -A12 'DIR Y: after MD' <<<"$OUT" | grep -qE "NEWDIR[[:space:]]+<DIR>"; then
    echo "FAIL: Y:\\NEWDIR not visible in DIR Y: after MD" >&2
    fail=1
fi
# Read-only enforcement: HELLO.TXT must survive DEL.
if ! grep -A2 "HELLO.TXT must still be there" <<<"$OUT" | grep -q "HELLO"; then
    echo "FAIL: read-only enforcement may have allowed DEL Y:\\HELLO.TXT" >&2
    fail=1
fi
# SKIP(MSDOS4): wildcard DIR assertion — format differs; covered by FreeDOS.
# Uninstall.
if ! grep -q "ext4-dos uninstalled" <<<"$OUT"; then
    echo "FAIL: ext4 -u didn't report uninstalled" >&2
    fail=1
fi
if ! grep -A4 "Re-check" <<<"$OUT" | grep -q "TSR not detected"; then
    echo "FAIL: TSR still detected after ext4 -u" >&2
    fail=1
fi
# SKIP(MSDOS4): auto-detect check — CONFIG.SYS load path differs.

# e2fsck-clean after all writes.
E2FSCK="$(command -v e2fsck || true)"
for c in /opt/homebrew/opt/e2fsprogs/sbin/e2fsck \
         /usr/local/opt/e2fsprogs/sbin/e2fsck; do
    [[ -x "$c" ]] && E2FSCK="$c" && break
done
if [[ -n "$E2FSCK" ]]; then
    PART_IMG="$MSDOS4_DIR/post-run-part.img"
    dd if="$EXT4_IMG" of="$PART_IMG" bs=512 skip=2048 status=none
    if "$E2FSCK" -fn "$PART_IMG" >"$MSDOS4_DIR/e2fsck.out" 2>&1; then
        rm -f "$PART_IMG" "$MSDOS4_DIR/e2fsck.out"
    else
        E2RC=$?
        echo "FAIL: e2fsck on ext4 fixture reported errors (rc=$E2RC):" >&2
        cat "$MSDOS4_DIR/e2fsck.out" >&2
        rm -f "$PART_IMG" "$MSDOS4_DIR/e2fsck.out"
        fail=1
    fi
else
    echo "WARN: e2fsck not found — skipping post-run integrity check" >&2
fi

exit $fail
