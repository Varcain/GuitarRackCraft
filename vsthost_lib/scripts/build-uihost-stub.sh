#!/usr/bin/env bash
# Builds uihost_stub.dll for x86_64 and i686 via mingw-w64. Implements
# CLSID_UIHostNoLaunch + ITipInvocation as no-ops so plugins that
# CoCreateInstance Windows' touch-keyboard host don't crash on the NULL
# they otherwise get from wine. See external/uihost_stub/uihost_stub.c.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"

ASSETS="$REPO/src/main/assets"
mkdir -p "$ASSETS"

build_one() {
    local arch="$1"      # x86_64 | i686
    local out_name="$2"  # uihost_stub_x64.dll | uihost_stub_x86.dll
    local cc="${arch}-w64-mingw32-gcc"

    if ! command -v "$cc" >/dev/null 2>&1; then
        echo "Error: $cc not found." >&2
        exit 1
    fi

    local out="$ASSETS/$out_name"
    "$cc" -O2 -Wall -Wextra -shared -static-libgcc \
        -o "$out" \
        "$REPO/external/uihost_stub/uihost_stub.c" \
        -lole32 -luuid
    "${cc%-gcc}-strip" "$out" 2>/dev/null || true
    file "$out"
    echo "Built $out ($(du -h "$out" | cut -f1))"
}

case "${1:-all}" in
    x86_64) build_one x86_64 uihost_stub_x64.dll ;;
    i686)   build_one i686   uihost_stub_x86.dll ;;
    all)
        build_one x86_64 uihost_stub_x64.dll
        build_one i686   uihost_stub_x86.dll
        ;;
    *)
        echo "usage: $0 [x86_64|i686|all]" >&2
        exit 2
        ;;
esac
