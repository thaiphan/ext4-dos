#!/usr/bin/env bash
# Boot MS-DOS 4 in DOSBox-X HEAVY DEBUGGER. Useful when our TSR's
# interaction with kernel internals goes sideways (rare these days, but the
# layout-sensitive corruption bugs we hit during bring-up needed it).
#
# Workflow once you're at the I-> debugger prompt:
#   1. AUTOEXEC runs `ext4chk /B` which issues INT 3 — that drops you into
#      the debugger automatically (no Pause-key needed; Macs don't have one).
#      Note the addresses ext4chk just printed.
#   2. `BPM <SEG>:<OFF>` to watch a memory location.
#   3. F5 (or Fn+F5 on a Mac) to resume.
#   4. When the BPM fires, the disassembly + register panes show the
#      writing instruction's CS:IP and registers — paste back to the
#      assistant for analysis.
set -euo pipefail

DOS_DIR="build/dos"
MSDOS4_DIR="tests/msdos4"
SOURCE_IMG="${MSDOS4_SRC:-tests/msdos4/msdos4-source.img}"
TEST_IMG="$MSDOS4_DIR/test.img"
EXT4_IMG="tests/images/disk.img"

# Prefer the in-tree heavy-debug build set up by scripts/setup-debugger.sh.
# Honor DEBUG_DOSBOX if the user pointed at a custom build.
DEBUG_DOSBOX="${DEBUG_DOSBOX:-external/dosbox-x/src/dosbox-x}"

if [[ ! -x "$DEBUG_DOSBOX" ]]; then
    echo "ERROR: heavy-debug DOSBox-X not found at $DEBUG_DOSBOX" >&2
    echo "  Run: bash scripts/setup-debugger.sh   (~10 min, one-off build)" >&2
    echo "  Or:  DEBUG_DOSBOX=/path/to/dosbox-x bash $0" >&2
    exit 1
fi
[[ -f "$SOURCE_IMG" ]] || { echo "ERROR: $SOURCE_IMG missing" >&2; exit 1; }
[[ -f "$EXT4_IMG"   ]] || { echo "ERROR: $EXT4_IMG missing"   >&2; exit 1; }
for f in ext4.exe ext4chk.exe ext4dir.exe ext4cnt.exe ext4dmp.exe; do
    [[ -x "$DOS_DIR/$f" ]] || { echo "ERROR: $DOS_DIR/$f missing" >&2; exit 1; }
done

mkdir -p "$MSDOS4_DIR"
cp "$SOURCE_IMG" "$TEST_IMG"

cat > "$MSDOS4_DIR/config.sys.tmp" <<'EOF'
LASTDRIVE=Z
INSTALL=A:\EXT4.EXE -q 0x81
EOF
awk 'BEGIN{ORS="\r\n"} {print}' "$MSDOS4_DIR/config.sys.tmp" > "$MSDOS4_DIR/config.sys"
rm -f "$MSDOS4_DIR/config.sys.tmp"

cat > "$MSDOS4_DIR/autoexec.bat.tmp" <<'EOF'
@echo off
echo --- TSR installed; printing addresses ---
A:\EXT4CHK.EXE /D /B
echo --- triggering bug ---
DIR Y:
echo --- DONE ---
EOF
awk 'BEGIN{ORS="\r\n"} {print}' "$MSDOS4_DIR/autoexec.bat.tmp" > "$MSDOS4_DIR/autoexec.bat"
rm -f "$MSDOS4_DIR/autoexec.bat.tmp"

mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4.exe"    ::EXT4.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4chk.exe" ::EXT4CHK.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4dir.exe" ::EXT4DIR.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4cnt.exe" ::EXT4CNT.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/ext4dmp.exe" ::EXT4DMP.EXE
mcopy -i "$TEST_IMG" -o "$MSDOS4_DIR/config.sys"   ::CONFIG.SYS
mcopy -i "$TEST_IMG" -o "$MSDOS4_DIR/autoexec.bat" ::AUTOEXEC.BAT

cat <<'BANNER'

DOSBox-X heavy debugger workflow:
  1. Wait for AUTOEXEC: ext4chk prints "&free_blocks_count = SEG:OFF"
     and issues INT 3 (drops into debugger automatically).
  2. At I-> prompt, set BPMs on all 4 bytes (uint64, byte-precise BPM):
         BPM 143b:08aa    (or whatever SEG:OFF was printed)
         BPM 143b:08ab
         BPM 143b:08ac
         BPM 143b:08ad
  3. F5 to resume.
  4. DIR Y: runs and triggers the corruption — debugger pauses with
     CS:IP of the writer in Code Overview.
  5. Paste back what you see.

BANNER

exec "$DEBUG_DOSBOX" -fastlaunch -nopromptfolder -debug \
    -set "dos break on int3=true" \
    -set "sdl quit warning=false" \
    -set "sdl windowresolution=1280x900" \
    -set "render aspect=false" \
    -c "imgmount A: $(pwd)/$TEST_IMG -t floppy" \
    -c "imgmount 3 $(pwd)/$EXT4_IMG -fs none -t hdd" \
    -c "boot A:"
