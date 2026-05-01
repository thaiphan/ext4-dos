#!/usr/bin/env bash
set -euo pipefail

OUT_DIR="tests/images"
OUT="$OUT_DIR/small.img"
SIZE_MB="${SIZE_MB:-16}"

MKFS="$(command -v mkfs.ext4 || true)"
if [[ -z "$MKFS" ]]; then
    MKFS="/opt/homebrew/opt/e2fsprogs/sbin/mkfs.ext4"
fi
if [[ ! -x "$MKFS" ]]; then
    echo "ERROR: mkfs.ext4 not found. Install via: brew install e2fsprogs" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1m count=0 seek="$SIZE_MB" status=none
"$MKFS" -F -L "ext4-dos-test" "$OUT" >/dev/null

echo "Wrote fixture: $OUT (${SIZE_MB} MiB)"
