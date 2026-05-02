#!/usr/bin/env bash
# Loads ext4.exe in DOSBox-X with an explicit drive letter, runs
# ext4chk.exe to verify install-check protocol, then uninstalls.
# Quick smoke test — doesn't boot a real DOS.
#
# Auto-pick coverage lives in freedos-test (which boots FreeDOS with a
# proper LASTDRIVE=Z so the MSCDEX-style scan has free slots). DOSBox-X's
# built-in DOS reports LASTDRIVE=1 and -set "dos lastdrive=z" doesn't
# move it, so the auto-pick path can't be exercised here.
set -euo pipefail

if ! command -v dosbox-x >/dev/null 2>&1; then
    echo "ERROR: dosbox-x not found. Install: brew install dosbox-x" >&2
    exit 1
fi

DOS_DIR="build/dos"
EXT4_IMG="tests/images/disk.img"

if [[ ! -f "$EXT4_IMG" ]]; then
    echo "ERROR: $EXT4_IMG not found. Run: make fixture-partitioned" >&2
    exit 1
fi
for f in ext4.exe ext4chk.exe; do
    if [[ ! -x "$DOS_DIR/$f" ]]; then
        echo "ERROR: $DOS_DIR/$f not found. Run: make dos-build" >&2
        exit 1
    fi
done

OUT="$DOS_DIR/out.txt"
LOG="$DOS_DIR/dosbox.log"
BAT="$DOS_DIR/tsrtest.bat"
rm -f "$OUT" "$LOG"

# DOSBox-X's -c chain only honors ~10 commands; everything past the
# imgmount/mount/cd setup goes in a batch file.
cat > "$BAT" <<'EOF'
@echo off
echo === BEFORE LOAD === > out.txt
ext4chk.exe >> out.txt
echo === LOADING TSR (EXT4 0x81 Y:) === >> out.txt
ext4.exe 0x81 Y: >> out.txt
echo === AFTER LOAD === >> out.txt
ext4chk.exe >> out.txt
echo === UNINSTALL === >> out.txt
ext4.exe -u >> out.txt
echo === FINAL CHECK === >> out.txt
ext4chk.exe >> out.txt
echo === DONE === >> out.txt
EOF

dosbox-x -fastlaunch -nopromptfolder -exit \
    -c "mount c $(pwd)/$DOS_DIR" \
    -c "imgmount 3 $(pwd)/$EXT4_IMG -fs none -t hdd" \
    -c "c:" \
    -c "tsrtest.bat" \
    >"$LOG" 2>&1

if [[ ! -f "$OUT" ]]; then
    echo "ERROR: no output captured. DOSBox-X log tail:" >&2
    tail -20 "$LOG" >&2
    exit 1
fi

cat "$OUT"

fail=0
if ! grep -q "ext4 mounted: drive 0x81" "$OUT"; then
    echo "FAIL: didn't mount drive 0x81 explicitly" >&2
    fail=1
fi
if ! grep -A2 "AFTER LOAD" "$OUT" | grep -q "TSR detected"; then
    echo "FAIL: TSR not detected after load" >&2
    fail=1
fi
if ! grep -q "ext4-dos uninstalled" "$OUT"; then
    echo "FAIL: uninstall didn't confirm" >&2
    fail=1
fi
notdetected_count=$(grep -c "TSR not detected" "$OUT" || true)
if [[ "$notdetected_count" -ne 2 ]]; then
    echo "FAIL: expected 'TSR not detected' 2x (pre-load, post-uninstall), got $notdetected_count" >&2
    fail=1
fi

rm -f "$OUT" "$BAT"
exit $fail
