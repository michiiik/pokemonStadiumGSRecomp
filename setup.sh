#!/usr/bin/env bash
# Initialize and verify this repository's committed public dependencies.

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR"

# N64Recomp contains an optional Ares gitlink whose historical object is not
# available from the public Ares repository. Initialize every other committed
# dependency recursively without making that optional oracle a prerequisite.
git -c submodule.ares-bridge/third_party/ares.update=none submodule update --init --recursive

DECOMP_DIR="$SCRIPT_DIR/disasm"
N64RECOMP_DIR="$SCRIPT_DIR/n64recomp"
RUNTIME_DIR="$SCRIPT_DIR/lib/N64ModernRuntime"
SDL2_DIR="$SCRIPT_DIR/lib/SDL2"
ADRENOTOOLS_DIR="$SCRIPT_DIR/lib/adrenotools"
RT64_DIR="$SCRIPT_DIR/lib/rt64"
UI_DIR="$SCRIPT_DIR/recomp-ui"
ARES_DIR="$N64RECOMP_DIR/ares-bridge/third_party/ares"

require_repo() {
    if [ ! -e "$1/.git" ] || ! git -C "$1" rev-parse --git-dir >/dev/null 2>&1; then
        echo "Error: required repository is missing: $1" >&2
        echo "Run 'git submodule update --init --recursive' from $SCRIPT_DIR." >&2
        exit 1
    fi
}

for dependency in "$DECOMP_DIR" "$N64RECOMP_DIR" "$RUNTIME_DIR" "$SDL2_DIR" "$ADRENOTOOLS_DIR" "$RT64_DIR" "$UI_DIR"; do
    require_repo "$dependency"
done

SHA=$(sed -n 's/^[[:space:]]*sha[[:space:]]*=[[:space:]]*//p' "$SCRIPT_DIR/n64recomp.pin" | tr -d '[:space:]')
if [ -z "$SHA" ]; then
    echo "Error: no sha found in n64recomp.pin" >&2
    exit 1
fi

ACTUAL=$(git -C "$N64RECOMP_DIR" rev-parse HEAD)
if [ "$ACTUAL" != "$SHA" ]; then
    echo "Note: n64recomp HEAD ($ACTUAL) differs from the committed pin ($SHA)."
fi

ROM_SOURCE="$SCRIPT_DIR/baserom.z64"
ROM_PATH="$DECOMP_DIR/baseroms/us/baserom.z64"
if [ -f "$ROM_SOURCE" ] && [ ! -f "$ROM_PATH" ]; then
    mkdir -p "$(dirname -- "$ROM_PATH")"
    cp "$ROM_SOURCE" "$ROM_PATH"
    echo "Staged baserom.z64 -> disasm/baseroms/us/"
fi

EXPECTED_MD5="1561c75d11cedf356a8ddb1a4a5f9d5d"
if [ -f "$ROM_PATH" ]; then
    if command -v md5sum >/dev/null 2>&1; then
        ACTUAL_MD5=$(md5sum "$ROM_PATH" | awk '{print $1}')
    else
        ACTUAL_MD5=$(md5 -q "$ROM_PATH")
    fi
    if [ "$ACTUAL_MD5" != "$EXPECTED_MD5" ]; then
        echo "Warning: Stadium 2 baserom MD5 is $ACTUAL_MD5; expected $EXPECTED_MD5." >&2
    else
        echo "Stadium 2 baserom MD5 OK."
    fi
else
    echo "Note: place your legal Stadium 2 US ROM at $ROM_SOURCE or $ROM_PATH"
fi

if [ "${WITH_ARES:-0}" = "1" ]; then
    if [ -e "$ARES_DIR/.git" ] && git -C "$ARES_DIR" rev-parse HEAD >/dev/null 2>&1; then
        echo "Ares checkout is available."
    else
        echo "Note: optional Ares gitlink is unavailable; continuing without it." >&2
    fi
fi

echo
echo "Repository dependencies are available."
echo "  pokestadiumgs: $(git -C "$DECOMP_DIR" rev-parse --short HEAD)"
echo "  N64Recomp:     $(git -C "$N64RECOMP_DIR" rev-parse --short HEAD)"
echo
echo "Build the disassembly from: $DECOMP_DIR"
echo "Configure the game from this repository:"
echo "  cmake -S . -B build"
