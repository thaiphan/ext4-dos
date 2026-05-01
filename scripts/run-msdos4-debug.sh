#!/usr/bin/env bash
# Boot the MS-DOS 4 testbed under DOSBox-X compiled with --enable-debug=heavy.
# Drops you at the DOS prompt with the TSR already installed (via CONFIG.SYS
# INSTALL=), so you can break into the debugger (Alt+Pause) and instrument
# INT 2Fh AH=11h dispatch by hand.
#
# Heavy debugger quick reference once Alt+Pause has frozen the guest:
#   BPINT 2F        break on next INT 2Fh (then check AH manually)
#   BPINT 21 31     break on INT 21h AH=31h (TSR exit) — useful for the
#                   batch-context _dos_keep corruption bug
#   BP <seg>:<off>  break on a code address
#   D <seg>:<off>   dump memory
#   R               show registers
#   F5              continue
#   F10 / F11       step over / step into
#   LOG / LOGS      start/stop CPU log to LOGCPU.TXT
#   INTLOG          toggle INT logging
#
# Wiki: https://dosbox-x.com/wiki/Debugger
#
# Env vars:
#   DOSBOX_X_DEBUG   path to debug-built binary
#                    (default: ../dosbox-x-src/src/dosbox-x)
#   MSDOS4_SRC       source floppy image
#                    (default: tests/msdos4/msdos4-source.img)
#   MODE             install | prompt
#                    install (default): TSR loaded via CONFIG.SYS — for
#                                       INT 2Fh redirector debugging
#                    prompt:            no TSR, plain MS-DOS 4 prompt — for
#                                       observing batch-context bugs by
#                                       hand-running TSR.EXE then watching
#                                       COMMAND.COM
set -euo pipefail

DOS_DIR="build/dos"
MSDOS4_DIR="tests/msdos4"
SOURCE_IMG="${MSDOS4_SRC:-tests/msdos4/msdos4-source.img}"
TEST_IMG="$MSDOS4_DIR/test-debug.img"
EXT4_IMG="tests/images/disk.img"
DOSBOX_X_DEBUG="${DOSBOX_X_DEBUG:-$(cd "$(dirname "$0")/.." && pwd)/../dosbox-x-src/src/dosbox-x}"
MODE="${MODE:-install}"

if [[ ! -x "$DOSBOX_X_DEBUG" ]]; then
    echo "ERROR: debug-built dosbox-x not found at: $DOSBOX_X_DEBUG" >&2
    echo "  Build it:  (cd ../dosbox-x-src && ./build-debug-macos-sdl2)" >&2
    echo "  Or set DOSBOX_X_DEBUG=<path>" >&2
    exit 1
fi
if [[ ! -f "$SOURCE_IMG" ]]; then
    echo "ERROR: $SOURCE_IMG not found." >&2
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

case "$MODE" in
    install)
        cat > "$MSDOS4_DIR/config.sys.tmp" <<'EOF'
LASTDRIVE=Z
INSTALL=A:\TSR.EXE -q 0x81
EOF
        ;;
    prompt)
        cat > "$MSDOS4_DIR/config.sys.tmp" <<'EOF'
LASTDRIVE=Z
EOF
        ;;
    *)
        echo "ERROR: unknown MODE=$MODE (expected: install | prompt)" >&2
        exit 1
        ;;
esac
awk 'BEGIN{ORS="\r\n"} {print}' "$MSDOS4_DIR/config.sys.tmp" > "$MSDOS4_DIR/config.sys"
rm -f "$MSDOS4_DIR/config.sys.tmp"

# Empty autoexec — we want to drop straight to a prompt so the user can
# set breakpoints in the debugger before triggering anything.
cat > "$MSDOS4_DIR/autoexec.bat.tmp" <<'EOF'
@echo off
EOF
awk 'BEGIN{ORS="\r\n"} {print}' "$MSDOS4_DIR/autoexec.bat.tmp" > "$MSDOS4_DIR/autoexec.bat"
rm -f "$MSDOS4_DIR/autoexec.bat.tmp"

mcopy -i "$TEST_IMG" -o "$DOS_DIR/tsr.exe"     ::TSR.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/tsr_chk.exe" ::TSR_CHK.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/tsr_dir.exe" ::TSR_DIR.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/tsr_cnt.exe" ::TSR_CNT.EXE
mcopy -i "$TEST_IMG" -o "$DOS_DIR/tsr_dmp.exe" ::TSR_DMP.EXE
mcopy -i "$TEST_IMG" -o "$MSDOS4_DIR/config.sys"   ::CONFIG.SYS
mcopy -i "$TEST_IMG" -o "$MSDOS4_DIR/autoexec.bat" ::AUTOEXEC.BAT

echo "===== DOSBox-X debug build ====="
echo "  binary:   $DOSBOX_X_DEBUG"
echo "  mode:     $MODE"
echo "  floppy:   $TEST_IMG"
echo "  ext4:     $EXT4_IMG (hdd 3 / drive 0x81)"
echo
echo "Once at the DOS prompt, hit Alt+Pause to break into the debugger."
echo "Type 'BPINT 2F' then F5 to trap the next INT 2Fh redirector dispatch."
echo

exec "$DOSBOX_X_DEBUG" \
    -nopromptfolder \
    -c "imgmount A: $(pwd)/$TEST_IMG -t floppy" \
    -c "imgmount 3 $(pwd)/$EXT4_IMG -fs none -t hdd" \
    -c "boot A:"
