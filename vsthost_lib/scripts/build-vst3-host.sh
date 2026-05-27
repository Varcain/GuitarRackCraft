#!/usr/bin/env bash
# Cross-compiles vst3_host.exe via mingw-w64 g++ (C++17). The VST3 SDK is
# C++ and uses templates heavily, so vst3_host can't share build infra with
# the C-based vst_host. The output goes to src/main/assets next to
# vst_host.exe; the launcher picks one based on plugin file extension.
#
# Only builds x86_64. 32-bit VST3 doesn't really exist anymore (Steinberg
# discontinued the 32-bit ABI), and our wine path is x86_64-only anyway.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"

ASSETS="$REPO/src/main/assets"
mkdir -p "$ASSETS"

CXX="x86_64-w64-mingw32-g++"
if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "Error: $CXX not found." >&2
    echo "  Install: sudo apt install g++-mingw-w64-x86-64" >&2
    exit 1
fi

SDK="$REPO/external/vst3sdk"
HOST_SRC="$REPO/external/vst_host_vst3/vst3_host.cpp"
OUT="$ASSETS/vst3_host.exe"

# SDK sources we need to compile in. Minimal set — just hosting helpers
# plus the IID definitions that resolve the COM interface identifiers.
# (We deliberately skip vstgui4 et al — host doesn't render GUI itself.)
SDK_SRCS=(
    "$SDK/public.sdk/source/vst/hosting/module_win32.cpp"
    "$SDK/public.sdk/source/vst/hosting/module.cpp"
    "$SDK/public.sdk/source/vst/hosting/hostclasses.cpp"
    "$SDK/public.sdk/source/vst/hosting/parameterchanges.cpp"
    "$SDK/public.sdk/source/vst/hosting/eventlist.cpp"
    "$SDK/public.sdk/source/vst/hosting/pluginterfacesupport.cpp"
    "$SDK/public.sdk/source/vst/hosting/processdata.cpp"
    "$SDK/public.sdk/source/vst/vstinitiids.cpp"
    "$SDK/public.sdk/source/common/commoniids.cpp"
    "$SDK/public.sdk/source/vst/utility/stringconvert.cpp"
    "$SDK/pluginterfaces/base/funknown.cpp"
    "$SDK/pluginterfaces/base/conststringtable.cpp"
    "$SDK/pluginterfaces/base/coreiids.cpp"
    "$SDK/base/source/fobject.cpp"
    "$SDK/base/source/fstring.cpp"
    "$SDK/base/source/baseiids.cpp"
    "$SDK/base/source/fbuffer.cpp"
    "$SDK/base/source/fdebug.cpp"
    "$SDK/base/source/updatehandler.cpp"
    "$SDK/base/thread/source/flock.cpp"
)

echo "Building $OUT ..."
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-multichar \
    -DUNICODE -D_UNICODE \
    -DSMTG_OS_WINDOWS=1 -DSMTG_OS_LINUX=0 -DSMTG_OS_MACOS=0 \
    -DRELEASE=1 \
    -I"$REPO/external" \
    -I"$SDK" \
    -I"$SDK/pluginterfaces" \
    -I"$SDK/public.sdk" \
    -I"$SDK/base" \
    -o "$OUT" \
    "$HOST_SRC" \
    "${SDK_SRCS[@]}" \
    -Wl,--stack,16777216 \
    -lole32 -loleaut32 -luuid -static -static-libgcc -static-libstdc++

"${CXX%-g++}-strip" "$OUT" 2>/dev/null || true
file "$OUT"
echo "Built $OUT ($(du -h "$OUT" | cut -f1))"
