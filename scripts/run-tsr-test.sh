#!/usr/bin/env bash
# Loads tsr.exe in DOSBox-X, then runs tsr_chk.exe to verify the TSR
# responds to its install-check protocol.
set -euo pipefail

if ! command -v dosbox-x >/dev/null 2>&1; then
    echo "ERROR: dosbox-x not found. Install: brew install dosbox-x" >&2
    exit 1
fi

DOS_DIR="build/dos"
for f in tsr.exe tsr_chk.exe; do
    if [[ ! -x "$DOS_DIR/$f" ]]; then
        echo "ERROR: $DOS_DIR/$f not found. Run: make dos-build" >&2
        exit 1
    fi
done

OUT="$DOS_DIR/out.txt"
LOG="$DOS_DIR/dosbox.log"
rm -f "$OUT" "$LOG"

dosbox-x -fastlaunch -nopromptfolder -exit \
    -c "mount c $(pwd)/$DOS_DIR" \
    -c "c:" \
    -c "echo === BEFORE LOAD === > out.txt" \
    -c "tsr_chk.exe >> out.txt" \
    -c "echo === LOADING TSR === >> out.txt" \
    -c "tsr.exe >> out.txt" \
    -c "echo === AFTER LOAD === >> out.txt" \
    -c "tsr_chk.exe >> out.txt" \
    >"$LOG" 2>&1

if [[ ! -f "$OUT" ]]; then
    echo "ERROR: no output captured. DOSBox-X log tail:" >&2
    tail -20 "$LOG" >&2
    exit 1
fi

cat "$OUT"
rm -f "$OUT"
