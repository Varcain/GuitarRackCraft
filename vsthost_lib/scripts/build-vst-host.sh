#!/usr/bin/env bash
# Cross-compiles vst_host.exe via mingw-w64. Builds both architectures by
# default (vst_host.exe = 64-bit, vst_host_x86.exe = 32-bit). Pass an arg
# to build only one: ./build-vst-host.sh x86_64  |  ./build-vst-host.sh i686
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"

ASSETS="$REPO/src/main/assets"
mkdir -p "$ASSETS"

build_one() {
    local arch="$1"          # x86_64 | i686
    local out_name="$2"      # vst_host.exe | vst_host_x86.exe
    local cc="${arch}-w64-mingw32-gcc"

    if ! command -v "$cc" >/dev/null 2>&1; then
        echo "Error: $cc not found." >&2
        if [ "$arch" = "i686" ]; then
            echo "  Install: sudo apt install gcc-mingw-w64-i686 binutils-mingw-w64-i686" >&2
        else
            echo "  Install: sudo apt install gcc-mingw-w64-x86-64" >&2
        fi
        exit 1
    fi

    local out="$ASSETS/$out_name"
    # Reproducible output (see build-wine-android.sh): -ffile-prefix-map drops
    # the checkout path, --no-insert-timestamp writes a fixed PE timestamp.
    # Strip at link time (-s) since a separate GNU strip pass stamps the
    # current time back in; --enable-long-section-names keeps ".eh_frame"
    # intact (ld otherwise truncates section names in stripped images, which
    # broke vst3_host.exe under wine/FEX).
    "$cc" -O2 -Wall -Wextra -Wno-unused-parameter \
        -s -ffile-prefix-map="$REPO"=/vsthost_lib \
        -Wl,--no-insert-timestamp -Wl,--enable-long-section-names \
        -I"$REPO/external/vst2" \
        -I"$REPO/external" \
        -o "$out" \
        "$REPO/external/vst_host/vst_host.c" \
        -Wl,--stack,16777216 \
        -lkernel32 -static
    file "$out"
    echo "Built $out ($(du -h "$out" | cut -f1))"
}

case "${1:-all}" in
    x86_64) build_one x86_64 vst_host.exe ;;
    i686)   build_one i686   vst_host_x86.exe ;;
    all)
        build_one x86_64 vst_host.exe
        build_one i686   vst_host_x86.exe
        ;;
    *)
        echo "usage: $0 [x86_64|i686|all]" >&2
        exit 2
        ;;
esac
