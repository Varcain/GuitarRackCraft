#!/usr/bin/env bash
# Builds FEX-Emu's two PE-side DLLs:
#   - libarm64ecfex.dll  (ARM64EC PE,    x86_64-emulation under arm64ec wine)
#   - libwow64fex.dll    (AArch64 PE,    x86-emulation    under arm64    wine)
#
# Both use llvm-mingw's clang as the cross compiler. Output lives at
# external/fex-upstream/build-{arm64ec,wow64}/{Bin/,Source/Windows/.../}.
#
# This is upstream FEX with no patches applied yet. The first run will reveal
# what (if anything) breaks for our toolchain version; fixes get applied via
# patch files in patches/fex/ and re-applied via apply-fex-patches.sh.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

export PATH="${repo_root}/external/llvm-mingw/install/bin:$PATH"

# Sanity checks.
which arm64ec-w64-mingw32-clang aarch64-w64-mingw32-clang > /dev/null || {
  echo "error: llvm-mingw not on PATH. Run scripts/setup-fex-pivot.sh first."
  exit 1
}
[ -d external/fex-upstream ] || {
  echo "error: external/fex-upstream missing. Run scripts/setup-fex-pivot.sh first."
  exit 1
}

build_one() {
  triple="$1"        # arm64ec-w64-mingw32  |  aarch64-w64-mingw32
  arch_flag="$2"     # ARCHITECTURE_arm64ec=ON | ARCHITECTURE_arm64=ON
  build_dir="$3"     # build-arm64ec | build-wow64

  echo "=== FEX build: ${triple} ==="
  mkdir -p "external/fex-upstream/${build_dir}"
  (cd "external/fex-upstream/${build_dir}" && \
    cmake -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=../Data/CMake/toolchain_mingw.cmake \
      -DMINGW_TRIPLE="${triple}" \
      -DENABLE_LTO=False \
      -DBUILD_TESTING=False \
      -DENABLE_JEMALLOC=False \
      -DENABLE_JEMALLOC_GLIBC_ALLOC=False \
      -D${arch_flag}=ON \
      .. && \
    ninja)
}

case "${1:-all}" in
  arm64ec) build_one arm64ec-w64-mingw32 ARCHITECTURE_arm64ec build-arm64ec ;;
  wow64)   build_one aarch64-w64-mingw32 ARCHITECTURE_arm64   build-wow64   ;;
  all)
    build_one arm64ec-w64-mingw32 ARCHITECTURE_arm64ec build-arm64ec
    build_one aarch64-w64-mingw32 ARCHITECTURE_arm64   build-wow64
    ;;
  *) echo "usage: $0 [arm64ec|wow64|all]"; exit 1 ;;
esac

echo
echo "=== outputs ==="
find external/fex-upstream/build-* -maxdepth 4 \
  \( -name "libarm64ecfex.dll" -o -name "libwow64fex.dll" \) \
  -printf '  %p  (%s bytes)\n' 2>/dev/null || true
