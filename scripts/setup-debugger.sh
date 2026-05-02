#!/usr/bin/env bash
# One-time setup: clone DOSBox-X and build it with the heavy-debugger UI
# enabled. Used for tracing MS-DOS 4 / FreeDOS kernel internals when our
# TSR's interaction with them goes sideways (BPM watchpoints, single-step,
# etc.).
#
# Pins to a known-good upstream commit so future builds are reproducible.
# Re-run this whenever you want to refresh — it's idempotent (skips clone /
# rebuild if the binary is already up to date).
set -euo pipefail

UPSTREAM="https://github.com/joncampbell123/dosbox-x.git"
PIN_COMMIT="280f4b1ec90b43b6e1193f1bad7950b1349a0ce4"
EXTERNAL="external/dosbox-x"
BIN="$EXTERNAL/src/dosbox-x"

case "$(uname -s)" in
    Darwin) BUILD_SCRIPT=build-debug-macos-sdl2 ;;
    Linux)  BUILD_SCRIPT=build-debug-sdl2 ;;
    *) echo "ERROR: setup not wired for $(uname -s) — see $UPSTREAM/blob/master/BUILD.md" >&2
       exit 1 ;;
esac

mkdir -p external

if [[ ! -d "$EXTERNAL/.git" ]]; then
    echo "==> Cloning DOSBox-X into $EXTERNAL ..."
    git clone --depth 50 "$UPSTREAM" "$EXTERNAL"
fi

cd "$EXTERNAL"
CURRENT=$(git rev-parse HEAD)
if [[ "$CURRENT" != "$PIN_COMMIT" ]]; then
    echo "==> Checking out pinned commit $PIN_COMMIT ..."
    git fetch origin "$PIN_COMMIT" || git fetch origin
    git checkout "$PIN_COMMIT"
fi
cd - >/dev/null

if [[ -x "$BIN" ]]; then
    BUILT_REV=$("$BIN" --version 2>&1 | head -1 || true)
    if grep -qi 'heavy' <(strings "$BIN" 2>/dev/null | grep -m1 -i heavy || true); then
        echo "==> $BIN already built with heavy debugger — skipping rebuild."
        echo "    ($BUILT_REV)"
        exit 0
    fi
fi

echo "==> Building heavy-debug DOSBox-X via $BUILD_SCRIPT ..."
echo "    (this takes ~5-10 minutes — autotools + internal SDL2 + zlib)"
cd "$EXTERNAL"
chmod +x "$BUILD_SCRIPT"
"./$BUILD_SCRIPT"
cd - >/dev/null

if [[ ! -x "$BIN" ]]; then
    echo "ERROR: build appeared to succeed but $BIN missing." >&2
    exit 1
fi

echo
echo "==> Done. Heavy-debug DOSBox-X built at $BIN"
echo "    Run: bash scripts/run-msdos4-debug.sh"
