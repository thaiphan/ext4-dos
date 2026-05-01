#!/usr/bin/env bash
# Runs build/dos/dos_cli.exe in DOSBox-X with tests/images/disk.img attached
# as INT 13h hard disk 0x80. Captures stdout via DOS shell redirection.
#
# Usage:
#   bash scripts/run-dosbox.sh           # superblock + features only
#   bash scripts/run-dosbox.sh 13        # also dump inode 13's first block
set -euo pipefail

if ! command -v dosbox-x >/dev/null 2>&1; then
    echo "ERROR: dosbox-x not found." >&2
    echo "  macOS:  brew install dosbox-x" >&2
    exit 1
fi

DOS_DIR="build/dos"
IMG="tests/images/disk.img"
EXE="$DOS_DIR/dos_cli.exe"

if [[ ! -x "$EXE" ]]; then
    echo "ERROR: $EXE not found. Run: make dos-build" >&2
    exit 1
fi
if [[ ! -f "$IMG" ]]; then
    echo "ERROR: $IMG not found. Run: make fixture-partitioned" >&2
    exit 1
fi

OUT="$DOS_DIR/out.txt"
LOG="$DOS_DIR/dosbox.log"
rm -f "$OUT" "$LOG"

INODE="${1:-}"
if [[ -n "$INODE" ]]; then
    DOS_CMD="dos_cli.exe 0x80 $INODE > out.txt"
else
    DOS_CMD="dos_cli.exe > out.txt"
fi

dosbox-x -fastlaunch -nopromptfolder -exit \
    -c "mount c $(pwd)/$DOS_DIR" \
    -c "imgmount 2 $(pwd)/$IMG -t hdd -fs none" \
    -c "c:" \
    -c "$DOS_CMD" \
    >"$LOG" 2>&1

if [[ ! -f "$OUT" ]]; then
    echo "ERROR: dos_cli.exe did not produce output. DOSBox-X log tail:" >&2
    tail -20 "$LOG" >&2
    exit 1
fi

cat "$OUT"
rm -f "$OUT"
