#!/usr/bin/env bash
# Cross-build libLLVM (AArch64 target) for Android Bionic arm64, as the foundation
# for Mesa lavapipe (the universal software-Vulkan fallback when no GPU Vulkan
# driver — Turnip on Adreno, the vendor Vulkan elsewhere — can run zink/DXVK).
#
# Version: LLVM 18.1.3 (mesa 24.2.8 supports LLVM ~15..18; LLVM 21 in
# external/llvm-mingw is too new for it). Source fetched into
# external/llvm-android/ (llvm + cmake + third-party from the 18.1.3 monorepo
# tarball). The HOST tablegen is /usr/bin/llvm-tblgen-18 (also 18.1.3) so no host
# LLVM build is needed.
#
# Minimal: AArch64 target only, no tools/tests/examples, static libs. Installs a
# CMake package (lib/cmake/llvm/LLVMConfig.cmake) so the mesa meson build can find
# the TARGET libs via the cmake method (a target llvm-config can't run on the host).
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"            # vsthost_lib/
L="$repo_root/external/llvm-android"
SRC="$L/llvm"
BUILD="$L/build-android-arm64"
PREFIX="$L/install-android-arm64"
HOST_TBLGEN="${HOST_LLVM_TBLGEN:-/usr/bin/llvm-tblgen-18}"

NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/26.1.10909125}"
API=28

[ -f "$SRC/CMakeLists.txt" ] || { echo "error: LLVM source not at $SRC (fetch llvm-project-18.1.3.src first)" >&2; exit 1; }
[ -x "$HOST_TBLGEN" ] || { echo "error: host llvm-tblgen not at $HOST_TBLGEN" >&2; exit 1; }
[ -f "$NDK/build/cmake/android.toolchain.cmake" ] || { echo "error: NDK toolchain not found ($NDK)" >&2; exit 1; }
command -v ninja >/dev/null || { echo "error: ninja not on PATH" >&2; exit 1; }

echo "=== LLVM 18.1.3 cross-build for android-arm64 (AArch64 target, minimal) ==="
echo "    host tblgen: $HOST_TBLGEN ($($HOST_TBLGEN --version | grep -oE 'LLVM version [0-9.]+'))"
echo "    NDK API: $API   install: $PREFIX"

# ANDROID_ALLOW_UNDEFINED_SYMBOLS=ON: the LLVMHello example MODULE plugin
# (lib/Transforms/Hello, built unconditionally) has by-design undefined symbols
# resolved when opt loads it; NDK's default -Wl,--no-undefined turns those into
# link errors. We build LLVM as static .a, so dropping --no-undefined only
# affects LLVM's own unused shared modules — real missing symbols still fail at
# the lavapipe link (which links the .a's and does not set this flag).
cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM="android-$API" \
  -DANDROID_ALLOW_UNDEFINED_SYMBOLS=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DLLVM_TABLEGEN="$HOST_TBLGEN" \
  -DLLVM_TARGETS_TO_BUILD=AArch64 \
  -DLLVM_HOST_TRIPLE=aarch64-linux-android \
  -DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-linux-android \
  -DLLVM_BUILD_TOOLS=OFF \
  -DLLVM_INCLUDE_TOOLS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_UTILS=OFF \
  -DLLVM_INCLUDE_DOCS=OFF \
  -DLLVM_ENABLE_BINDINGS=OFF \
  -DLLVM_ENABLE_ZLIB=OFF \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_LIBEDIT=OFF \
  -DLLVM_ENABLE_LIBPFM=OFF \
  -DLLVM_ENABLE_THREADS=ON \
  -DLLVM_ENABLE_PIC=ON \
  -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DLLVM_BUILD_LLVM_DYLIB=OFF \
  -DLLVM_LINK_LLVM_DYLIB=OFF

echo "[+] ninja (this is the long pole, ~1-2h)"
ninja -C "$BUILD"
echo "[+] install → $PREFIX"
rm -rf "$PREFIX"
ninja -C "$BUILD" install

echo "[=] LLVM 18.1.3 android-arm64 installed:"
echo "    LLVMConfig.cmake: $(ls "$PREFIX"/lib/cmake/llvm/LLVMConfig.cmake 2>/dev/null && echo OK || echo MISSING)"
echo "    libs: $(ls "$PREFIX"/lib/libLLVM*.a 2>/dev/null | wc -l) static archives"
