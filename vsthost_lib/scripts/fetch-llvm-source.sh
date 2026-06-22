#!/usr/bin/env bash
# Fetch the LLVM 18.1.3 source that build-llvm-android.sh cross-compiles for
# lavapipe (the universal software-Vulkan fallback). llvmpipe is an LLVM-JIT
# software rasterizer, so the mesa lavapipe build links target libLLVM.
#
# Downloads the three component source tarballs (llvm + cmake + third-party) from
# the official LLVM 18.1.3 GitHub release and lays them out as
#   external/llvm-android/{llvm,cmake,third-party}/
# which is the layout LLVM's CMake expects (cmake/ and third-party/ are siblings
# of llvm/). The component tarballs (~130 MB total, vs the full monorepo) carry
# exactly what the minimal AArch64 build needs.
#
# Idempotent: a component already present is left untouched. Run once before
# scripts/build-llvm-android.sh; build-llvm-android.sh also calls this on demand.
#
# Why not a submodule: only 3 of the monorepo's subdirs are needed and the source
# is a fixed release, so a pinned download is lighter than vendoring the whole
# llvm-project history.
set -euo pipefail

VER="${LLVM_VERSION:-18.1.3}"
repo_root="$(cd "$(dirname "$0")/.." && pwd)"          # vsthost_lib/
L="$repo_root/external/llvm-android"
BASE="https://github.com/llvm/llvm-project/releases/download/llvmorg-$VER"

command -v curl >/dev/null || { echo "error: curl not on PATH" >&2; exit 1; }
command -v tar  >/dev/null || { echo "error: tar not on PATH" >&2; exit 1; }
command -v xz   >/dev/null || { echo "error: xz not on PATH (apt install xz-utils)" >&2; exit 1; }

mkdir -p "$L"

fetch() {
    local comp="$1" dest="$L/$1"
    if [ -e "$dest/CMakeLists.txt" ] || [ -e "$dest/Modules" ] || [ -e "$dest/benchmark" ]; then
        echo "[=] $comp/ already present — skip"
        return
    fi
    local tarball="$comp-$VER.src.tar.xz"
    local tmp; tmp="$L/.$tarball.part"
    echo "[+] downloading $tarball"
    curl -fL --retry 3 --retry-delay 2 -o "$tmp" "$BASE/$tarball"
    echo "[+] extracting → external/llvm-android/$comp/"
    rm -rf "$L/$comp-$VER.src"
    tar -C "$L" -xf "$tmp"
    mv "$L/$comp-$VER.src" "$dest"
    rm -f "$tmp"
}

echo "=== fetch LLVM $VER source → $L ==="
fetch llvm
fetch cmake
fetch third-party

echo "[=] LLVM $VER source ready (llvm/ cmake/ third-party/)."
echo "    next: scripts/build-llvm-android.sh  (host needs llvm-tblgen-$( echo "$VER" | cut -d. -f1 ); apt install llvm-$( echo "$VER" | cut -d. -f1 ))"
