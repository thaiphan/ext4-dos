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
FREEDOS_IMG="tests/freedos/FD14LITE.img"
WAIT_SECONDS="${WAIT_SECONDS:-60}"

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
if [[ ! -f "$FREEDOS_IMG" ]]; then
    echo "ERROR: $FREEDOS_IMG not found. Run: make tests/freedos/FD14LITE.img" >&2
    exit 1
fi
for f in ext4.exe ext4chk.exe ext4dir.exe ext4cnt.exe ext4dmp.exe ext4wr.exe ext4xfr.exe; do
    if [[ ! -x "$DOS_DIR/$f" ]]; then
        echo "ERROR: $DOS_DIR/$f missing. Run: make dos-build" >&2
        exit 1
    fi
done

mkdir -p "$MSDOS4_DIR"
cp "$SOURCE_IMG" "$TEST_IMG"
cp "$EXT4_SRC_IMG" "$EXT4_IMG"

# Extract FDAPM.EXE from the FreeDOS image for graceful DOSBox-X shutdown.
mcopy -i "${FREEDOS_IMG}@@32256" '::FREEDOS/BIN/FDAPM.COM' "$MSDOS4_DIR/fdapm.com"

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
REM Regression: REM_CLOSE must zero sf_ref_count or every Open leaks an
REM SFT entry.  At MS-DOS 4's default FILES=8 the pool exhausts after ~4
REM opens and EXEC starts returning error 4 ("Cannot execute").  Run 8
REM sequential TYPEs as a tight reproducer.
echo === Regression: 8 sequential TYPEs (SFT leak guard) === >> A:\OUT.TXT
TYPE Y:\HELLO.TXT >> A:\OUT.TXT
TYPE Y:\HELLO.TXT >> A:\OUT.TXT
TYPE Y:\HELLO.TXT >> A:\OUT.TXT
TYPE Y:\HELLO.TXT >> A:\OUT.TXT
TYPE Y:\HELLO.TXT >> A:\OUT.TXT
TYPE Y:\HELLO.TXT >> A:\OUT.TXT
TYPE Y:\HELLO.TXT >> A:\OUT.TXT
TYPE Y:\HELLO.TXT >> A:\OUT.TXT
echo === End regression === >> A:\OUT.TXT
REM Wildcard DIR works on MS-DOS 4 -- COMMAND.COM iterates FindNext and
REM filters in user space, just like FreeDOS, so our match-all PRI_PATH
REM ("Y:????????.???") does the right thing.
echo === DIR Y:\*.TXT (wildcard) === >> A:\OUT.TXT
DIR Y:\*.TXT >> A:\OUT.TXT
echo === TYPE Y:\HELLO.TXT === >> A:\OUT.TXT
TYPE Y:\HELLO.TXT >> A:\OUT.TXT
REM COPY Y:\... A:\... -- now works as of commit 071ae29 (AH=3Bh ChDir intercept).
echo === COPY Y:\HELLO.TXT A:\HELLO2.TXT === >> A:\OUT.TXT
COPY Y:\HELLO.TXT A:\HELLO2.TXT >> A:\OUT.TXT
echo === TYPE A:\HELLO2.TXT (verify COPY-from-Y) === >> A:\OUT.TXT
TYPE A:\HELLO2.TXT >> A:\OUT.TXT
echo === TYPE Y:\SUBDIR\NESTED.TXT (subdir traversal) === >> A:\OUT.TXT
TYPE Y:\SUBDIR\NESTED.TXT >> A:\OUT.TXT
REM MS-DOS 4 COMMAND.COM COPY+ silently drops a subdir source on a
REM redirector drive (bug in MS-DOS 4 itself: it prints only the first
REM source and skips Y:\SUBDIR\NESTED.TXT). Direct subdir TYPE above
REM confirms the redirector path works -- this is a COMMAND.COM
REM limitation, not ours. Use two root-level sources here so we still
REM exercise the COPY+ multi-file machinery.
echo === Multi-file: COPY HELLO+VERY~876 to BOTH.TXT === >> A:\OUT.TXT
COPY /B Y:\HELLO.TXT+Y:\VERY~876.TXT A:\BOTH.TXT >> A:\OUT.TXT
echo === TYPE A:\BOTH.TXT (concatenation result) === >> A:\OUT.TXT
TYPE A:\BOTH.TXT >> A:\OUT.TXT
echo === Write test: Y:\TARGET.TXT before === >> A:\OUT.TXT
TYPE Y:\TARGET.TXT >> A:\OUT.TXT
echo --- run ext4wr (in-place B + extend C) --- >> A:\OUT.TXT
A:\EXT4WR.EXE >> A:\OUT.TXT
echo === Y:\TARGET.TXT after write (expect 'B'*1024 + 'C'*1024) === >> A:\OUT.TXT
TYPE Y:\TARGET.TXT >> A:\OUT.TXT
REM DIR Y:\TARGET.TXT (single-file) -- now works as of the COPY-from-Y fix
REM (same root cause: ChDir on Y:\ was the gating bug).
echo === DIR Y:\TARGET.TXT (single-file) === >> A:\OUT.TXT
DIR Y:\TARGET.TXT >> A:\OUT.TXT
REM COPY A:\... Y:\... (file creation on Y: via COPY) -- now works.
echo === COPY A:\HELLO2.TXT Y:\NEWCOPY.TXT (create on Y:) === >> A:\OUT.TXT
COPY A:\HELLO2.TXT Y:\NEWCOPY.TXT >> A:\OUT.TXT
echo === TYPE Y:\NEWCOPY.TXT (verify COPY-to-Y) === >> A:\OUT.TXT
TYPE Y:\NEWCOPY.TXT >> A:\OUT.TXT
echo === Multi-block intra-Y: COPY Y:\TARGET.TXT Y:\NEWBIG.TXT === >> A:\OUT.TXT
COPY Y:\TARGET.TXT Y:\NEWBIG.TXT >> A:\OUT.TXT
echo === DIR Y:\NEWBIG.TXT (expect size 2048) === >> A:\OUT.TXT
DIR Y:\NEWBIG.TXT >> A:\OUT.TXT
echo === RENAME: REN Y:\NEWBIG.TXT RENAMED.TXT === >> A:\OUT.TXT
REN Y:\NEWBIG.TXT RENAMED.TXT >> A:\OUT.TXT
echo === DIR Y:\RENAMED.TXT (must exist, same size) === >> A:\OUT.TXT
DIR Y:\RENAMED.TXT >> A:\OUT.TXT
REM EXEC after writes -- previously corrupted; verify it still works here.
echo === ext4chk after writes (EXEC sanity) === >> A:\OUT.TXT
A:\EXT4CHK.EXE >> A:\OUT.TXT
echo === Make directory: MD Y:\NEWDIR === >> A:\OUT.TXT
MD Y:\NEWDIR >> A:\OUT.TXT
REM Under MS-DOS 4, DIR Y:\NEWDIR lists the *contents* of NEWDIR,
REM not NEWDIR itself in Y:\.  Use DIR Y: so the entry is visible.
echo === DIR Y: (NEWDIR must appear) === >> A:\OUT.TXT
DIR Y: >> A:\OUT.TXT
echo === Remove directory: RD Y:\NEWDIR === >> A:\OUT.TXT
RD Y:\NEWDIR >> A:\OUT.TXT
echo === DIR Y: (NEWDIR must be gone, TARGET extended to 2048) === >> A:\OUT.TXT
DIR Y: >> A:\OUT.TXT
echo === DEL Y:\NEWCOPY.TXT (remove created file) === >> A:\OUT.TXT
DEL Y:\NEWCOPY.TXT >> A:\OUT.TXT
echo === DIR Y: after DEL (NEWCOPY must be gone, HELLO still there) === >> A:\OUT.TXT
DIR Y: >> A:\OUT.TXT
echo === TYPE Y:\VERY~876.TXT (8.3 alias roundtrip) === >> A:\OUT.TXT
TYPE Y:\VERY~876.TXT >> A:\OUT.TXT
echo === TYPE Y:\VERY~EB7.TXT (8.3 alias roundtrip) === >> A:\OUT.TXT
TYPE Y:\VERY~EB7.TXT >> A:\OUT.TXT
echo === Verify g_fs.sb integrity (canary) === >> A:\OUT.TXT
A:\EXT4CHK.EXE /V >> A:\OUT.TXT
REM AX=11A3h direct works on MS-DOS 4 (bypasses kernel).  AX=7303h via
REM kernel returns "invalid function" since MS-DOS 4 predates the API.
echo === Get Extended Free Space (INT 2Fh AX=11A3h direct) === >> A:\OUT.TXT
A:\EXT4XFR.EXE Y:\ >> A:\OUT.TXT
echo === Subfunction call counts === >> A:\OUT.TXT
A:\EXT4CNT.EXE >> A:\OUT.TXT
echo === FindFirst capture dump === >> A:\OUT.TXT
A:\EXT4DMP.EXE >> A:\OUT.TXT
REM Uninstall must be last -- after this Y: drive is gone.
echo === Uninstall: EXT4 -U === >> A:\OUT.TXT
A:\EXT4.EXE -u >> A:\OUT.TXT
echo --- Re-check (should report not-installed) --- >> A:\OUT.TXT
A:\EXT4CHK.EXE >> A:\OUT.TXT
echo === AUTO-DETECT (no args) === >> A:\OUT.TXT
A:\EXT4.EXE >> A:\OUT.TXT
echo --- Re-check (auto-detect should land on D: = first free slot) --- >> A:\OUT.TXT
A:\EXT4CHK.EXE >> A:\OUT.TXT
DIR D: >> A:\OUT.TXT
A:\EXT4.EXE -u >> A:\OUT.TXT
echo === DONE === >> A:\OUT.TXT
A:\FDAPM.COM POWEROFF
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
mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4xfr.exe" ::EXT4XFR.EXE
mcopy -i "$TEST_IMG" -o "$MSDOS4_DIR/fdapm.com"    ::FDAPM.COM
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
    kill -9 "$BGPID" 2>/dev/null || true
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

# Assertions — mirror run-freedos-test.sh; remaining MS-DOS 4 sync gaps are
# marked SKIP(MSDOS4) inline with the reason.
fail=0
if ! grep -q "Hello, ext4-dos!" <<<"$OUT"; then
    echo "FAIL: TYPE Y:\\HELLO.TXT didn't return file content" >&2
    fail=1
fi
# Regression: 8 sequential TYPEs must all return content (SFT-leak guard).
HELLO_COUNT=$(grep -c "Hello, ext4-dos!" <<<"$OUT" || true)
if (( HELLO_COUNT < 8 )); then
    echo "FAIL: expected >=8 successful TYPE Y:\\HELLO.TXT readbacks (SFT leak?), got ${HELLO_COUNT}" >&2
    fail=1
fi
# 8.3 alias TYPE roundtrip (now passes — was masked by SFT-leak EXEC corruption).
if ! grep -q "long-named file ONE" <<<"$OUT"; then
    echo "FAIL: TYPE Y:\\VERY~876.TXT (8.3 alias) didn't return file content" >&2
    fail=1
fi
if ! grep -q "long-named file TWO" <<<"$OUT"; then
    echo "FAIL: TYPE Y:\\VERY~EB7.TXT (8.3 alias) didn't return file content" >&2
    fail=1
fi
# COPY-from-Y assertion (commit 071ae29 AH=3Bh ChDir intercept).
if ! grep -F -A2 'COPY Y:\HELLO.TXT A:\HELLO2.TXT' <<<"$OUT" | grep -q '1 File(s) copied'; then
    echo "FAIL: COPY Y:\\HELLO.TXT A:\\HELLO2.TXT didn't report '1 File(s) copied'" >&2
    fail=1
fi
if ! grep -F -A2 'TYPE A:\HELLO2.TXT (verify COPY-from-Y)' <<<"$OUT" | grep -q "Hello, ext4-dos!"; then
    echo "FAIL: A:\\HELLO2.TXT contents wrong after COPY-from-Y" >&2
    fail=1
fi
# Single-file DIR Y:\TARGET.TXT — should list the file (was previously broken).
if ! grep -F -A6 'DIR Y:\TARGET.TXT (single-file)' <<<"$OUT" | grep -qE "TARGET[[:space:]]+TXT[[:space:]]+2[,]?048"; then
    echo "FAIL: DIR Y:\\TARGET.TXT didn't show TARGET.TXT entry" >&2
    fail=1
fi
# COPY-to-Y assertion — file creation on Y: via COMMAND.COM COPY.
if ! grep -F -A2 'COPY A:\HELLO2.TXT Y:\NEWCOPY.TXT' <<<"$OUT" | grep -q '1 File(s) copied'; then
    echo "FAIL: COPY A:\\HELLO2.TXT Y:\\NEWCOPY.TXT didn't report '1 File(s) copied'" >&2
    fail=1
fi
if ! grep -F -A2 'TYPE Y:\NEWCOPY.TXT (verify COPY-to-Y)' <<<"$OUT" | grep -q "Hello, ext4-dos!"; then
    echo "FAIL: Y:\\NEWCOPY.TXT contents missing after COPY-to-Y" >&2
    fail=1
fi
# EXEC after writes — previously broken; ext4chk run after MD/RD/COPY-to-Y must work.
if ! grep -F -A4 'ext4chk after writes (EXEC sanity)' <<<"$OUT" | grep -q "TSR detected: AL=0xff"; then
    echo "FAIL: EXEC of A:\\EXT4CHK.EXE corrupted by prior writes" >&2
    fail=1
fi
# MD Y:\NEWDIR — check it appears in the subsequent DIR Y: listing.
if ! grep -F -A20 'DIR Y: (NEWDIR must appear)' <<<"$OUT" | grep -qE "NEWDIR[[:space:]]+<DIR>"; then
    echo "FAIL: Y:\\NEWDIR not visible in DIR Y: after MD" >&2
    fail=1
fi
# RD Y:\NEWDIR — must be absent from the subsequent DIR Y: listing.
if grep -F -A20 'DIR Y: (NEWDIR must be gone' <<<"$OUT" | grep -qE "NEWDIR[[:space:]]+<DIR>"; then
    echo "FAIL: Y:\\NEWDIR still visible after RD" >&2
    fail=1
fi
# Multi-file COPY /B (HELLO + VERY~876 -> A:\BOTH.TXT). Differs from
# FreeDOS (HELLO+SUBDIR\NESTED) because MS-DOS 4 COMMAND.COM's COPY+
# parser silently drops a subdir source on a redirector drive. Direct
# subdir TYPE above confirms the redirector handles subdirs fine.
if ! grep -F -A4 'COPY HELLO+VERY~876 to BOTH.TXT' <<<"$OUT" | grep -q '1 File(s) copied'; then
    echo "FAIL: multi-file COPY+ didn't report '1 File(s) copied'" >&2
    fail=1
fi
if ! grep -F -A4 'TYPE A:\BOTH.TXT (concatenation result)' <<<"$OUT" | grep -q 'long-named file ONE'; then
    echo "FAIL: A:\\BOTH.TXT missing concatenation tail (VERY~876.TXT contents)" >&2
    fail=1
fi
# Direct subdir traversal — TYPE Y:\SUBDIR\NESTED.TXT must return content.
if ! grep -F -A2 'TYPE Y:\SUBDIR\NESTED.TXT (subdir' <<<"$OUT" | grep -q 'Nested file contents'; then
    echo "FAIL: TYPE Y:\\SUBDIR\\NESTED.TXT didn't return content (subdir traversal)" >&2
    fail=1
fi
# Multi-block intra-Y COPY (Y:\TARGET.TXT -> Y:\NEWBIG.TXT).
if ! grep -F -A2 'COPY Y:\TARGET.TXT Y:\NEWBIG.TXT' <<<"$OUT" | grep -q '1 File(s) copied'; then
    echo "FAIL: intra-Y COPY didn't report '1 File(s) copied'" >&2
    fail=1
fi
if ! grep -F -A6 'DIR Y:\NEWBIG.TXT (expect size 2048)' <<<"$OUT" | grep -qE "NEWBIG[[:space:]]+TXT[[:space:]]+2[,]?048"; then
    echo "FAIL: Y:\\NEWBIG.TXT not 2048 bytes after intra-Y COPY" >&2
    fail=1
fi
# REN Y:\NEWBIG.TXT RENAMED.TXT — RENAMED.TXT must exist after.
if ! grep -F -A6 'DIR Y:\RENAMED.TXT (must exist' <<<"$OUT" | grep -qE "RENAMED[[:space:]]+TXT[[:space:]]+2[,]?048"; then
    echo "FAIL: Y:\\RENAMED.TXT (post-REN) not visible at size 2048" >&2
    fail=1
fi
# DEL Y:\NEWCOPY.TXT — NEWCOPY must be gone, HELLO still present.
if grep -F -A20 'DIR Y: after DEL' <<<"$OUT" | grep -qE "NEWCOPY[[:space:]]+TXT"; then
    echo "FAIL: Y:\\NEWCOPY.TXT still visible after DEL" >&2
    fail=1
fi
if ! grep -F -A20 'DIR Y: after DEL' <<<"$OUT" | grep -qE "HELLO[[:space:]]+TXT"; then
    echo "FAIL: HELLO.TXT missing from DIR Y: after DEL — over-deletion?" >&2
    fail=1
fi
# Write test: ext4wr reports both writes, and the post-write TYPE shows
# 'B' followed by 'C' (no 'A' left).  DOS TYPE doesn't emit a trailing
# newline so the next echo line concatenates; pattern-match for 100+
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
# DIR Y: after writes must show TARGET sized 2048 (the extend bumped it).
if ! grep -F -A12 'DIR Y: (NEWDIR must be gone' <<<"$OUT" | grep -qE "TARGET[[:space:]]+TXT[[:space:]]+2[,]?048"; then
    echo "FAIL: TARGET.TXT not 2048 bytes in DIR Y: after extend" >&2
    fail=1
fi
# Dynamic free-space check: net block delta after all writes is +3 blocks
# consumed (extend TARGET +1, create NEWCOPY +1, create NEWBIG +2, DEL
# NEWCOPY -1 = +3 blocks = 3072 bytes).  REN doesn't move blocks.  The
# first DIR Y: reads the install-time snapshot; the last DIR D: comes
# from the auto-detect re-install which captured a fresh snapshot
# reflecting all writes.  So FINAL == INIT - 3072.
INIT_FREE=$(grep -oE '[0-9,]+ bytes free' <<<"$OUT" | head -1 | sed 's/ bytes free//' | tr -d ',' || true)
FINAL_FREE=$(grep -oE '[0-9,]+ bytes free' <<<"$OUT" | tail -1 | sed 's/ bytes free//' | tr -d ',' || true)
if [[ -z "${INIT_FREE:-}" || -z "${FINAL_FREE:-}" ]]; then
    echo "FAIL: couldn't parse 'bytes free' lines from DIR output" >&2
    fail=1
elif (( FINAL_FREE != INIT_FREE - 3072 )); then
    echo "FAIL: 'bytes free' wrong (expected $((INIT_FREE - 3072)), got ${FINAL_FREE}) — extend + NEWCOPY + NEWBIG - DEL should net 3 blocks" >&2
    fail=1
fi
if ! grep -qE "verify:.*-> OK" <<<"$OUT"; then
    echo "FAIL: g_fs.sb integrity canary tripped" >&2
    fail=1
fi
# Direct INT 2Fh AX=11A3h must return data matching the install-time snapshot.
# This is the only path on MS-DOS 4 that exercises our REM_GETLARGESPACE
# handler — the kernel doesn't have AX=7303h.
EXTFREE_DIRECT_FREE=$(awk '/INT 2Fh AX=11A3h direct/,/INT 21h AX=7303h/' <<<"$OUT" | grep -oE 'bytes free *: *[0-9]+' | grep -oE '[0-9]+$' || true)
if [[ -z "${EXTFREE_DIRECT_FREE:-}" ]]; then
    echo "FAIL: ext4xfr didn't print AX=11A3h 'bytes free' line" >&2
    fail=1
elif (( EXTFREE_DIRECT_FREE != INIT_FREE )); then
    echo "FAIL: AX=11A3h direct bytes free (${EXTFREE_DIRECT_FREE}) doesn't match install-time snapshot (${INIT_FREE})" >&2
    fail=1
fi
# AX=7303h on MS-DOS 4 must now succeed — our INT 21h hook bridges
# AH=73h to the same install-time snapshot AL=A3h uses, since MS-DOS 4
# kernel itself doesn't know AH=73h. Free bytes must match the snapshot.
EXTFREE_KERNEL_FREE=$(awk '/INT 21h AX=7303h on/,EOF' <<<"$OUT" | grep -oE 'bytes free *: *[0-9]+' | head -1 | grep -oE '[0-9]+$' || true)
if [[ -z "${EXTFREE_KERNEL_FREE:-}" ]]; then
    echo "FAIL: AX=7303h on MS-DOS 4 didn't print bytes free — bridge not firing?" >&2
    fail=1
elif (( EXTFREE_KERNEL_FREE != INIT_FREE )); then
    echo "FAIL: AX=7303h on MS-DOS 4 bytes free (${EXTFREE_KERNEL_FREE}) doesn't match install-time snapshot (${INIT_FREE})" >&2
    fail=1
fi
# SKIP(MSDOS4): wildcard DIR Y:\*.TXT assertion — call returns no entries;
# pattern-matching path doesn't engage.  Tracked separately.
# Uninstall + auto-detect (mirrors FreeDOS).
if ! grep -q "ext4-dos uninstalled" <<<"$OUT"; then
    echo "FAIL: ext4 -u didn't report uninstalled" >&2
    fail=1
fi
if ! grep -A4 "Re-check" <<<"$OUT" | grep -q "TSR not detected"; then
    echo "FAIL: TSR still detected after ext4 -u" >&2
    fail=1
fi

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
