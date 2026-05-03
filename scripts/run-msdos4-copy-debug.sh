#!/usr/bin/env bash
# Heavy-debugger session for the MS-DOS 4 COPY-from-Y bug.
#
# Background: Our ext4prb.exe probe (added in 6de948d) confirms that an
# ExtOpen + GetTimes + GetXA + IOCTL + Read sequence works correctly when
# issued by a standalone EXE.  But the MS-DOS 4 internal COPY command
# (running inside COMMAND.COM) does ExtOpen + Close with NO Read between,
# even though it issues the SAME INT 21h calls.  Something in COMMAND.COM's
# environment makes the kernel-side flow diverge -- and we can't find it
# from outside.
#
# This BAT triggers two paths in the same boot:
#   1. ext4prb /c (the WORKING path -- baseline)
#   2. COPY Y:\HELLO.TXT A:\HELLO2.TXT (the BROKEN path)
#
# After the probe, ext4chk /B issues INT 3 -- the heavy debugger pauses
# automatically.  At that point you have a known-good state and can set
# breakpoints in MS-DOS 4 kernel code, then F5 to let COPY run and see
# where it diverges.
#
# Suggested breakpoints to try (these are MS-DOS 4 source labels that
# need to be resolved to runtime addresses by examining your binaries):
#
#   - $READ entry (HANDLE.ASM:343) -- if this hits during COPY, COPY DID
#     issue an AH=3Fh read.  If not, COPY bailed earlier.
#
#   - DOS_READ entry (DISK.ASM:144) -- the kernel-side read dispatcher.
#     Read access check at line 165-169 (CMP AL, open_for_write) -- step
#     through to see if access is being denied.
#
#   - filetimes_ok (HANDLE.ASM:664) -- AX=5700h Get File Times handler.
#     CheckOwner call at line 665.  If CheckOwner fails for COPY's handle,
#     $File_Times bails to LSeekError and COPY sees CF=1.
#
#   - CheckOwner (HANDLE.ASM:766) -- compares User_ID with sf_UID.
#     Watch what user_id and sf_UID are at this point.
#
# Workflow at the I-> prompt:
#   D ?cs:?ip   ; show current location
#   BPI 1c00:0123  ; set instruction breakpoint
#   F5  (or Fn+F5 on Mac) to resume
#   When breakpoint fires, examine registers and memory
#   Paste back what you see for analysis.
set -euo pipefail

DOS_DIR="build/dos"
MSDOS4_DIR="tests/msdos4"
SOURCE_IMG="${MSDOS4_SRC:-tests/msdos4/msdos4-source.img}"
TEST_IMG="$MSDOS4_DIR/test.img"
EXT4_IMG="$MSDOS4_DIR/test-ext4.img"
EXT4_SRC_IMG="tests/images/disk.img"
FREEDOS_IMG="tests/freedos/FD14LITE.img"
DEBUG_DOSBOX="${DEBUG_DOSBOX:-external/dosbox-x/src/dosbox-x}"

if [[ ! -x "$DEBUG_DOSBOX" ]]; then
    echo "ERROR: heavy-debug DOSBox-X not found at $DEBUG_DOSBOX" >&2
    echo "  Run: bash scripts/setup-debugger.sh" >&2
    exit 1
fi
[[ -f "$SOURCE_IMG" ]]    || { echo "ERROR: $SOURCE_IMG missing" >&2; exit 1; }
[[ -f "$EXT4_SRC_IMG" ]]  || { echo "ERROR: $EXT4_SRC_IMG missing" >&2; exit 1; }
[[ -f "$FREEDOS_IMG" ]]   || { echo "ERROR: $FREEDOS_IMG missing -- need it for FDAPM" >&2; exit 1; }
for f in ext4.exe ext4chk.exe ext4cnt.exe ext4dmp.exe ext4prb.exe; do
    [[ -x "$DOS_DIR/$f" ]] || { echo "ERROR: $DOS_DIR/$f missing -- run make dos-build" >&2; exit 1; }
done

mkdir -p "$MSDOS4_DIR"
cp "$SOURCE_IMG"   "$TEST_IMG"
cp "$EXT4_SRC_IMG" "$EXT4_IMG"
mcopy -i "${FREEDOS_IMG}@@32256" '::FREEDOS/BIN/FDAPM.COM' "$MSDOS4_DIR/fdapm.com"

printf 'LASTDRIVE=Z\r\nINSTALL=A:\\EXT4.EXE -q 0x81 Y:\r\n' > "$MSDOS4_DIR/config.sys"

cat > "$MSDOS4_DIR/autoexec.bat.tmp" <<'EOF'
@echo off
echo === BASELINE: ext4prb /c (works correctly) ===
A:\EXT4PRB.EXE /c
echo === DROPPING INTO DEBUGGER NOW ===
A:\EXT4CHK.EXE /B
echo --- after debugger break ---
echo === REPRO: actual COPY (broken) ===
COPY Y:\HELLO.TXT A:\HELLO2.TXT
echo --- after copy ---
A:\EXT4DMP.EXE
echo --- counts ---
A:\EXT4CNT.EXE
echo === DONE ===
A:\FDAPM.COM POWEROFF
EOF
awk 'BEGIN{ORS="\r\n"} {print}' "$MSDOS4_DIR/autoexec.bat.tmp" > "$MSDOS4_DIR/autoexec.bat"
rm -f "$MSDOS4_DIR/autoexec.bat.tmp"

mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4.exe"    ::EXT4.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4chk.exe" ::EXT4CHK.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4cnt.exe" ::EXT4CNT.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4dmp.exe" ::EXT4DMP.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4prb.exe" ::EXT4PRB.EXE
mcopy -i "$TEST_IMG" -o "$MSDOS4_DIR/fdapm.com"    ::FDAPM.COM
mcopy -i "$TEST_IMG" -o "$MSDOS4_DIR/config.sys"   ::CONFIG.SYS
mcopy -i "$TEST_IMG" -o "$MSDOS4_DIR/autoexec.bat" ::AUTOEXEC.BAT

cat <<'BANNER'

==============================================================================
HEAVY-DEBUGGER SESSION FOR COPY-FROM-Y BUG
==============================================================================

When the debugger pauses (after ext4prb /c succeeds, before COPY runs):

  1. The probe just demonstrated that ExtOpen + intermediate calls + Read
     all WORK correctly.  Note any addresses ext4chk printed.

  2. Set instruction breakpoints at MS-DOS 4 kernel landmarks.  You'll
     need to find their runtime addresses first -- one approach:

       D ?fs:0  to dump the IVT
       D ?cs:?ip  to see where you are now

       Then look at INT 21h vector (offset 21h*4 = 84h):
         D ?0:0084
       That gives you the segment of the DOS kernel.

       Find $READ within DOSGROUP (HANDLE.ASM:343).
       Find DOS_READ within DOSGROUP (DISK.ASM:144).
       Find $File_Times within DOSGROUP (HANDLE.ASM:580).
       Find CheckOwner within DOSGROUP (HANDLE.ASM:766).

  3. BPI at CheckOwner -- when COPY runs and calls AX=5700h Get File
     Times, this should fire.  At the breakpoint:

       Examine ES:DI -> SFT (the handle's SFT)
       Examine [User_ID] -- DOS kernel's current User_ID
       Examine ES:[DI+sf_UID] -- the SFT's UID (offset 0x2F)

       If they don't match, CheckOwner returns CF=1, $File_Times errors,
       COPY sees the error and bails.

  4. Alternatively BPI at $READ -- if this NEVER fires during COPY, COPY
     bailed before reaching the read.  Check what error it returned to
     COMMAND.COM (look at AX after the failing call).

  5. Press F5 (Fn+F5 on Mac) to resume.  Paste back what fires and what
     register/memory values you see.

==============================================================================

BANNER

exec "$DEBUG_DOSBOX" -fastlaunch -nopromptfolder -debug \
    -set "dos break on int3=true" \
    -set "sdl quit warning=false" \
    -set "sdl windowresolution=1280x900" \
    -set "render aspect=false" \
    -c "imgmount A: $(pwd)/$TEST_IMG -t floppy" \
    -c "imgmount 3 $(pwd)/$EXT4_IMG -fs none -t hdd" \
    -c "boot A:"
