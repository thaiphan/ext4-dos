#!/usr/bin/env bash
# One-time setup: downloads MS-DOS 6.22 from the Internet Archive, extracts
# the three installer floppy images, creates a blank virtual HDD, then
# launches DOSBox-X so you can complete the installation interactively.
#
# After you close DOSBox-X the resulting image is saved as
# tests/msdos6/msdos6-source.img and used by all future 'make msdos6-test' runs.
#
# Prerequisites:
#   - dosbox-x on PATH
#   - 7z on PATH  (macOS: brew install p7zip)
#   - curl on PATH
set -euo pipefail

MSDOS6_DIR="tests/msdos6"
DISKS_DIR="$MSDOS6_DIR/disks"
SOURCE_IMG="$MSDOS6_DIR/msdos6-source.img"
ARCHIVE_LOCAL="$MSDOS6_DIR/installer.7z"
# Note: "Enchanced" is the uploader's spelling — matches the actual filename.
ARCHIVE_URL="https://archive.org/download/ms-dos-6.22-with-enchanced-tools-floppy-disks_20231027/MS-DOS%206.22%20with%20Enchanced%20Tools%20Floppy%20Disks.7z"
HDD_SIZE_MB=64   # large enough for MS-DOS 6.22 + tools; FDISK rounds down

if [[ -f "$SOURCE_IMG" ]]; then
    echo "==> $SOURCE_IMG already exists — nothing to do."
    echo "    Delete it and re-run to redo the installation."
    exit 0
fi

if ! command -v dosbox-x >/dev/null 2>&1; then
    echo "ERROR: dosbox-x not found." >&2; exit 1
fi
if ! command -v 7z >/dev/null 2>&1; then
    echo "ERROR: 7z not found. Install with: brew install p7zip" >&2; exit 1
fi

mkdir -p "$DISKS_DIR"

# ── Step 1: download ──────────────────────────────────────────────────────────
if [[ ! -f "$ARCHIVE_LOCAL" ]]; then
    echo "==> Downloading MS-DOS 6.22 installer from Internet Archive..."
    curl -L --progress-bar -o "$ARCHIVE_LOCAL" "$ARCHIVE_URL"
else
    echo "==> Installer archive already downloaded."
fi

# ── Step 2: extract floppy images ─────────────────────────────────────────────
IMG_COUNT=$(find "$DISKS_DIR" -maxdepth 1 -iname "*.img" | wc -l | tr -d ' ')
if [[ "$IMG_COUNT" -lt 3 ]]; then
    echo "==> Extracting floppy images..."
    7z e -o"$DISKS_DIR" "$ARCHIVE_LOCAL" "*.img" -y
fi

# Find the three disk images by sort order (Disk1, Disk2, Disk3).
mapfile -t DISKS < <(find "$DISKS_DIR" -maxdepth 1 -iname "*.img" | sort)
if [[ "${#DISKS[@]}" -lt 3 ]]; then
    echo "ERROR: expected 3 .img files in $DISKS_DIR, found ${#DISKS[@]}." >&2
    ls -1 "$DISKS_DIR"
    exit 1
fi
DISK1="${DISKS[0]}"
DISK2="${DISKS[1]}"
DISK3="${DISKS[2]}"
echo "==> Floppy images: $(basename "$DISK1")  $(basename "$DISK2")  $(basename "$DISK3")"

# ── Step 3: blank HDD image ───────────────────────────────────────────────────
echo "==> Creating blank ${HDD_SIZE_MB} MB HDD image..."
dd if=/dev/zero of="$SOURCE_IMG" bs=1M count="$HDD_SIZE_MB" status=none

# ── Step 4: interactive install ───────────────────────────────────────────────
cat <<'MSG'

==> Launching DOSBox-X for MS-DOS 6.22 installation.

    In the DOSBox-X window:
      1. MS-DOS SETUP will start automatically.
      2. Press ENTER to accept Express Setup defaults.
      3. When asked for Disk 2 or Disk 3, press Ctrl+F4 in the DOSBox-X
         window to cycle to the next floppy image, then press ENTER.
      4. When setup says "Installation Complete" or asks you to restart,
         close the DOSBox-X window instead of pressing ENTER.
         (If you allow the machine to reboot, setup will restart from
          Disk 1 — just close the window at that point.)

    After you close DOSBox-X this script will finish automatically.

MSG

dosbox-x -fastlaunch -nopromptfolder \
    -c "imgmount A: $(pwd)/$DISK1 $(pwd)/$DISK2 $(pwd)/$DISK3 -t floppy" \
    -c "imgmount 2 $(pwd)/$SOURCE_IMG -fs none -t hdd" \
    -c "boot A:"

# dosbox-x runs in the foreground above; we reach here only after the window
# closes.  Verify the image looks like a real installation.
HDD_SIZE=$(stat -f%z "$SOURCE_IMG" 2>/dev/null || stat -c%s "$SOURCE_IMG")
if [[ "$HDD_SIZE" -lt $(( HDD_SIZE_MB * 1024 * 1024 )) ]]; then
    echo "ERROR: $SOURCE_IMG appears truncated (${HDD_SIZE} bytes)." >&2
    exit 1
fi

echo
echo "==> Installation complete. msdos6-source.img is ready."
echo "    Run 'make msdos6-test' to verify."
