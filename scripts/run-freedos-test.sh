#!/usr/bin/env bash
# Boots a fresh copy of the FreeDOS LiteUSB image with our TSR + test
# binaries injected, runs the tests via FDAUTO.BAT, then reads the
# captured OUT.TXT back out of the image.
#
# DOSBox-X doesn't exit on fdapm poweroff so we kill it after a timeout —
# DOS-level writes flush through to the image file before then, so the
# OUT.TXT mread afterward is reliable.
set -euo pipefail

DOS_DIR="build/dos"
FREEDOS_DIR="tests/freedos"
SOURCE_IMG="$FREEDOS_DIR/FD14LITE.img"
TEST_IMG="$FREEDOS_DIR/test.img"
PARTITION_OFFSET=32256
WAIT_SECONDS="${WAIT_SECONDS:-25}"

if [[ ! -f "$SOURCE_IMG" ]]; then
    echo "ERROR: $SOURCE_IMG not found." >&2
    echo "  Download:  curl -L -o $SOURCE_IMG.zip 'https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.4/FD14-LiteUSB.zip'" >&2
    echo "  Extract:   unzip -d $FREEDOS_DIR $SOURCE_IMG.zip FD14LITE.img" >&2
    exit 1
fi

for f in tsr.exe tsr_chk.exe tsr_dir.exe; do
    if [[ ! -x "$DOS_DIR/$f" ]]; then
        echo "ERROR: $DOS_DIR/$f missing. Run: make dos-build" >&2
        exit 1
    fi
done

# Fresh copy of the FreeDOS image — never touch the source.
cp "$SOURCE_IMG" "$TEST_IMG"

# Custom FDAUTO.BAT: skip the FreeDOS installer chain and run our TSR test
# sequence with output captured to C:\OUT.TXT.
cat > "$FREEDOS_DIR/fdauto-test.bat" <<'EOF'
@echo off
SET PATH=C:\FREEDOS\BIN
echo === BEFORE TSR === > C:\OUT.TXT
C:\TSR_CHK.EXE >> C:\OUT.TXT
echo === LOAD TSR === >> C:\OUT.TXT
C:\TSR.EXE >> C:\OUT.TXT
echo === AFTER TSR === >> C:\OUT.TXT
C:\TSR_CHK.EXE >> C:\OUT.TXT
echo === FindFirst Y: === >> C:\OUT.TXT
C:\TSR_DIR.EXE >> C:\OUT.TXT
echo === DONE === >> C:\OUT.TXT
fdapm poweroff
EOF

# Inject everything.
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/tsr.exe"     ::TSR.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/tsr_chk.exe" ::TSR_CHK.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$DOS_DIR/tsr_dir.exe" ::TSR_DIR.EXE
mcopy -i "$TEST_IMG@@$PARTITION_OFFSET" -o "$FREEDOS_DIR/fdauto-test.bat" ::FDAUTO.BAT

# Boot in DOSBox-X, then kill after timeout (poweroff doesn't exit DOSBox-X).
LOG="$FREEDOS_DIR/test.log"
rm -f "$LOG"

dosbox-x -fastlaunch -nopromptfolder -exit \
    -c "imgmount 2 $(pwd)/$TEST_IMG -fs none -t hdd" \
    -c "boot c:" \
    >"$LOG" 2>&1 &
BGPID=$!

# Wait until DOSBox-X exits OR our timeout elapses.
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

# Pull the captured output back out.
echo
echo "===== OUT.TXT (from FreeDOS) ====="
mtype -i "$TEST_IMG@@$PARTITION_OFFSET" ::OUT.TXT 2>/dev/null || echo "(no OUT.TXT in image)"
