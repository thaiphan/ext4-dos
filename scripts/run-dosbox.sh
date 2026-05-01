#!/usr/bin/env bash
# Runs build/dos/dos_cli.exe in DOSBox-X with tests/images/disk.img attached
# as INT 13h hard disk 0x80. Captures dos_cli.exe's stdout via DOS shell
# redirection into a file on the mounted C: drive, then prints it.
set -euo pipefail

if ! command -v dosbox-x >/dev/null 2>&1; then
    echo "ERROR: dosbox-x not found." >&2
    echo "  macOS:  brew install dosbox-x" >&2
    echo "  Linux:  use your distro's package manager" >&2
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

dosbox-x -fastlaunch -nopromptfolder -exit \
    -c "mount c $(pwd)/$DOS_DIR" \
    -c "imgmount 2 $(pwd)/$IMG -t hdd -fs none" \
    -c "c:" \
    -c "dos_cli.exe > out.txt" \
    >"$LOG" 2>&1

if [[ ! -f "$OUT" ]]; then
    echo "ERROR: dos_cli.exe did not produce output. DOSBox-X log tail:" >&2
    tail -20 "$LOG" >&2
    exit 1
fi

cat "$OUT"
rm -f "$OUT"
