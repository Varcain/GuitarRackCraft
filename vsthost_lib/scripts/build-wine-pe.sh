#!/usr/bin/env bash
# Builds wine's PE side (591 ARM64X DLLs) on x86_64 Linux host.
# This pass is what produces the .dll files that ship inside the APK
# alongside the Android-cross-compiled Unix-side wine binary.
#
# Output: external/wine-upstream/build-arm64ec/dlls/<name>/aarch64-windows/<name>.dll
#
# The Unix-side host binaries this build also produces (loader/wine,
# server/wineserver) are x86_64 Linux ELF — they're throwaway. The
# Android-Bionic wine binary comes from scripts/build-wine-android.sh.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

# --- apply Bionic / FEX-pivot patches to clean upstream wine ----------------
# Same patch set as build-wine-android.sh — wine source is shared between
# both builds. Idempotent reset + apply. See apply-wine-patches.sh.
"$(dirname "$0")/apply-wine-patches.sh"

export PATH="${repo_root}/external/llvm-mingw/install/bin:$PATH"

build_dir="external/wine-upstream/build-arm64ec"
mkdir -p "$build_dir"
cd "$build_dir"

if [ ! -f Makefile ]; then
  echo "=== configure wine PE-side (arm64ec + aarch64) ==="
  # --disable-win16: avoid wine's 16-bit emulation layer. Two reasons:
  # (1) FEX-Emu doesn't translate 16-bit code, so even if we built it the
  #     plugins couldn't use it. (2) llvm-mingw's clang 21.1-rc2 crashes
  #     compiling dlls/krnl386.exe16/selector.c (16-bit segment register
  #     inline asm). Disabling the whole subsystem sidesteps the bug.
  ../configure \
    --enable-archs=arm64ec,aarch64,i386 \
    --with-mingw=clang \
    --disable-win16 \
    --without-x \
    --with-freetype \
    --without-alsa \
    --without-pulse \
    --without-vulkan \
    --without-opengl \
    --without-fontconfig \
    --disable-tests
fi

echo "=== make (~10 min on 12 cores) ==="
make -j"$(nproc)"

echo
echo "=== PE outputs ==="
echo "  ntdll.dll:      $(ls -la dlls/ntdll/aarch64-windows/ntdll.dll | awk '{print $5}') bytes"
echo "  total .dll:     $(find dlls -path '*/aarch64-windows/*.dll' | wc -l)"
