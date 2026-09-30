#!/usr/bin/env bash
# Fetch the LLVM 18.1.3 source that build-llvm-android.sh cross-compiles for
# lavapipe (the universal software-Vulkan fallback). llvmpipe is an LLVM-JIT
# software rasterizer, so the mesa lavapipe build links target libLLVM.
#
# Downloads the three component source tarballs (llvm + cmake + third-party) from
# the official LLVM 18.1.3 GitHub release and lays them out as
#   external/llvm-android/{llvm,cmake,third-party}/
# which is the layout LLVM's CMake expects (cmake/ and third-party/ are siblings
# of llvm/). The component tarballs (~60 MB total, vs the full monorepo) carry
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
. "$repo_root/scripts/lib/common.sh"                    # fetch_verified

# sha256 of each component tarball, per version.
declare -A SHA256=(
    [llvm-18.1.3]=fa6db8951f5ef576ac6bad43d5e1ed83962754538c998fbfa0397cd4521abc00
    [cmake-18.1.3]=acfecb615d41c5b1a0a31e15324994ca06f7a3f37d8958d719b20de0d217b71b
    [third-party-18.1.3]=ba1de46e740133d361c0d5d1387befa309f0b60f81bc2bf003252bebdcf9eada
)
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
    local sha256="${SHA256[$comp-$VER]:-}"
    [ -n "$sha256" ] || { echo "error: no sha256 pinned for $tarball (add it to SHA256)" >&2; exit 1; }
    fetch_verified "$BASE/$tarball" "$sha256" "$L/.$tarball"
    echo "[+] extracting → external/llvm-android/$comp/"
    rm -rf "$L/$comp-$VER.src"
    tar -C "$L" -xf "$L/.$tarball"
    mv "$L/$comp-$VER.src" "$dest"
    rm -f "$L/.$tarball"
}

echo "=== fetch LLVM $VER source → $L ==="
fetch llvm
fetch cmake
fetch third-party

echo "[=] LLVM $VER source ready (llvm/ cmake/ third-party/)."
echo "    next: scripts/build-llvm-android.sh  (host needs llvm-tblgen-$( echo "$VER" | cut -d. -f1 ); apt install llvm-$( echo "$VER" | cut -d. -f1 ))"
