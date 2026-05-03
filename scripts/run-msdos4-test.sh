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
REM SKIP(MSDOS4): DIR Y:\*.TXT wildcard returns no entries on MS-DOS 4.
REM Diagnosis from EXT4DMP per-call captures:
REM   PRI_PATH for the wildcard arrives as "Y:????????.???" -- MS-DOS 4 has
REM   wildcard-expanded the literal ".TXT" extension into "???".  Our
REM   pattern compile sees all '?'s and treats it as match-all; FindFirst
REM   returns the first entry (lost+found) which DOS then rejects against
REM   its own internal pattern check (it knows the user typed *.TXT).  The
REM   actual FCB-format pattern (????????TXT) is presumably in the SDB at
REM   SDA+TMP_DM+DM_NAME_PAT but a naive read there returns garbage on the
REM   first call (DOS doesn't pre-fill it for redirector dispatch).  Need
REM   either: a heuristic to detect "valid FCB pattern" in SDB, or a parse
REM   of MS-DOS 4 source's IFS dispatch to find where it stashes the
REM   literal extension.  FreeDOS preserves it in PRI_PATH and works.
echo === DIR Y:\*.TXT (wildcard) === >> A:\OUT.TXT
DIR Y:\*.TXT >> A:\OUT.TXT
echo === TYPE Y:\HELLO.TXT === >> A:\OUT.TXT
TYPE Y:\HELLO.TXT >> A:\OUT.TXT
REM SKIP(MSDOS4): COPY /B Y:\HELLO.TXT+Y:\SUBDIR\NESTED.TXT A:\BOTH.TXT --
REM MS-DOS 4 COPY opens the remote file (AL=0x2E) but never calls our AL=0x08
REM REM_READ; it uses some other read path we don't claim.  BOTH.TXT comes out
REM empty.  Same applies to the multi-block COPY Y:\TARGET.TXT A:\NEWBIG.TXT.
echo === Write test: Y:\TARGET.TXT before === >> A:\OUT.TXT
TYPE Y:\TARGET.TXT >> A:\OUT.TXT
echo --- run ext4wr (in-place B + extend C) --- >> A:\OUT.TXT
A:\EXT4WR.EXE >> A:\OUT.TXT
echo === Y:\TARGET.TXT after write (expect 'B'*1024 + 'C'*1024) === >> A:\OUT.TXT
TYPE Y:\TARGET.TXT >> A:\OUT.TXT
REM SKIP(MSDOS4): DIR Y:\TARGET.TXT shows volume header only on MS-DOS 4 --
REM single-file DIR doesn't list the entry.  Verify size via the normal DIR Y:
REM listing further down instead.
REM SKIP(MSDOS4): COPY A:\... Y:\... (file creation on Y: via COPY) -- MS-DOS 4
REM COMMAND.COM refuses writes to remote drives at the OS level (returns
REM "0 File(s) copied"), so NEWCOPY.TXT/NEWBIG.TXT/RENAMED.TXT/DEL all hit
REM that wall.  File creation on Y: is reachable only via direct INT 21h
REM (which is what ext4wr does).
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
echo === TYPE Y:\VERY~876.TXT (8.3 alias roundtrip) === >> A:\OUT.TXT
TYPE Y:\VERY~876.TXT >> A:\OUT.TXT
echo === TYPE Y:\VERY~EB7.TXT (8.3 alias roundtrip) === >> A:\OUT.TXT
TYPE Y:\VERY~EB7.TXT >> A:\OUT.TXT
echo === Verify g_fs.sb integrity (canary) === >> A:\OUT.TXT
A:\EXT4CHK.EXE /V >> A:\OUT.TXT
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
# SKIP(MSDOS4): NEWCOPY content / NEWBIG size / RENAMED size — depend on
# COPY-to-Y file creation, refused by MS-DOS 4 COMMAND.COM (architectural).
# MD Y:\NEWDIR — check it appears in the subsequent DIR Y: listing.
if ! grep -F -A12 'DIR Y: (NEWDIR must appear)' <<<"$OUT" | grep -qE "NEWDIR[[:space:]]+<DIR>"; then
    echo "FAIL: Y:\\NEWDIR not visible in DIR Y: after MD" >&2
    fail=1
fi
# RD Y:\NEWDIR — must be absent from the subsequent DIR Y: listing.
if grep -F -A12 'DIR Y: (NEWDIR must be gone' <<<"$OUT" | grep -qE "NEWDIR[[:space:]]+<DIR>"; then
    echo "FAIL: Y:\\NEWDIR still visible after RD" >&2
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
# Dynamic free-space check: extend consumed exactly one block (1024 bytes).
# The first DIR Y: reads the install-time snapshot; the last DIR D: comes from
# the auto-detect re-install which captured a fresh snapshot reflecting the
# write.  So FINAL == INIT - 1024.
INIT_FREE=$(grep -oE '[0-9,]+ bytes free' <<<"$OUT" | head -1 | sed 's/ bytes free//' | tr -d ',' || true)
FINAL_FREE=$(grep -oE '[0-9,]+ bytes free' <<<"$OUT" | tail -1 | sed 's/ bytes free//' | tr -d ',' || true)
if [[ -z "${INIT_FREE:-}" || -z "${FINAL_FREE:-}" ]]; then
    echo "FAIL: couldn't parse 'bytes free' lines from DIR output" >&2
    fail=1
elif (( FINAL_FREE != INIT_FREE - 1024 )); then
    echo "FAIL: 'bytes free' wrong (expected $((INIT_FREE - 1024)), got ${FINAL_FREE}) — extend may not be consuming a fs block" >&2
    fail=1
fi
if ! grep -qE "verify:.*-> OK" <<<"$OUT"; then
    echo "FAIL: g_fs.sb integrity canary tripped" >&2
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
