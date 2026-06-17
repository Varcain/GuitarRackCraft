#!/usr/bin/env bash
# Build the Khronos Vulkan loader (libvulkan.so.1) from source for arm64
# Bionic, replacing the Termux `vulkan-loader-generic` prebuilt that
# fetch-turnip-libs.sh used to pull. This is Phase 1 of the source-migration
# (retire the fetch-* prebuilts); see docs/.
#
# WHY a "generic" loader (not the Android platform one): the DXVK D3D11 path
# dlopens this loader from wine's win32u (SONAME_LIBVULKAN, redirected via env)
# and points it at Turnip through VK_ICD_FILENAMES → freedreno_icd.aarch64.json.
# That env/file-based ICD discovery is the loader's Linux/Unix codepath, NOT the
# Android HAL (hw_get_module) path. Upstream KhronosGroup/Vulkan-Loader has NO
# Android-HAL code and NO __ANDROID__ gating at all — discovery is gated on the
# COMMON_UNIX_PLATFORMS macro (satisfied by __linux__, which the NDK clang always
# defines) and on CMake's CMAKE_SYSTEM_NAME. The stock android.toolchain.cmake
# sets CMAKE_SYSTEM_NAME=Android → CMakeLists.txt FATAL_ERRORs "Android build not
# supported!". The fix (exactly what Termux's vulkan-loader-generic does) is to
# build with the NDK clang/sysroot but force -DCMAKE_SYSTEM_NAME=Linux — no source
# patch. That selects the Unix discovery + loader_linux.c device-sort, and the
# SOVERSION/OUTPUT_NAME machinery emits SONAME libvulkan.so.1 linking only
# dl/m/pthread (all Bionic).
#
# Output: vsthost_lib/toolchain/turnip-libs/libvulkan.so.1 (a REAL file, the
# version symlink dereferenced — the runtime tar extractor skips symlinks and
# win32u's dlopen + Turnip's NEEDED reference the bare SONAME). phase_turnip then
# bundles it into src/main/assets/turnip-libs.tar.gz alongside the Turnip ICD/HAL.
#
# Called by scripts/build-all.sh (phase "turnip", after fetch-turnip-libs.sh);
# also runnable standalone. Run after:
#   git submodule update --init vsthost_lib/external/Vulkan-Loader vsthost_lib/external/Vulkan-Headers

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"            # vsthost_lib/
cd "$repo_root"

LOADER_SRC="$repo_root/external/Vulkan-Loader"
HEADERS_SRC="$repo_root/external/Vulkan-Headers"
OUT="$repo_root/toolchain/turnip-libs"
BUILD="$repo_root/.cache/vulkan-loader-build"
HDR_INSTALL="$BUILD/headers-install"

NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/26.1.10909125}"
NDKBIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
API=28                                                   # matches build-mesa-zink (the GPU/Vulkan runtime group)

# --- prerequisites ---------------------------------------------------------
[ -f "$LOADER_SRC/CMakeLists.txt" ] || {
    echo "error: Vulkan-Loader submodule not at $LOADER_SRC" >&2
    echo "  run: git submodule update --init vsthost_lib/external/Vulkan-Loader" >&2
    exit 1; }
[ -d "$HEADERS_SRC/include/vulkan" ] || {
    echo "error: Vulkan-Headers submodule not at $HEADERS_SRC" >&2
    echo "  run: git submodule update --init vsthost_lib/external/Vulkan-Headers" >&2
    exit 1; }
[ -x "$NDKBIN/aarch64-linux-android$API-clang" ] || {
    echo "error: NDK not found at $NDK (set ANDROID_NDK)" >&2; exit 1; }
command -v cmake >/dev/null || { echo "error: cmake not on PATH" >&2; exit 1; }
GEN="Unix Makefiles"; command -v ninja >/dev/null && GEN="Ninja"

SYSROOT="$NDKBIN/../sysroot"

echo "=== Vulkan loader build (libvulkan.so.1, generic/Linux discovery on Bionic) ==="
echo "    loader:  $(git -C "$LOADER_SRC" describe --tags 2>/dev/null || echo '?')"
echo "    headers: $(git -C "$HEADERS_SRC" describe --tags 2>/dev/null || echo '?')"
echo "    NDK API: $API   generator: $GEN"

# --- 1. install Vulkan-Headers (header-only) so find_package(VulkanHeaders CONFIG) resolves ---
echo "[+] install Vulkan-Headers → $HDR_INSTALL"
rm -rf "$BUILD"
cmake -S "$HEADERS_SRC" -B "$BUILD/headers" -G "$GEN" >/dev/null
cmake --install "$BUILD/headers" --prefix "$HDR_INSTALL" >/dev/null

# --- 2. configure the loader: NDK clang/sysroot, but CMAKE_SYSTEM_NAME=Linux ---
# (do NOT use android.toolchain.cmake — it sets ANDROID=1 → FATAL_ERROR). WSI off
# (headless: no X11/Wayland on device; surface extensions come from the ICD).
echo "[+] cmake configure (CMAKE_SYSTEM_NAME=Linux, WSI off)"
cmake -S "$LOADER_SRC" -B "$BUILD/loader" -G "$GEN" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER="$NDKBIN/aarch64-linux-android$API-clang" \
  -DCMAKE_CXX_COMPILER="$NDKBIN/aarch64-linux-android$API-clang++" \
  -DCMAKE_AR="$NDKBIN/llvm-ar" \
  -DCMAKE_RANLIB="$NDKBIN/llvm-ranlib" \
  -DCMAKE_SYSROOT="$SYSROOT" \
  -DCMAKE_FIND_ROOT_PATH="$SYSROOT;$HDR_INSTALL" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
  -DCMAKE_PREFIX_PATH="$HDR_INSTALL" \
  -DVULKAN_HEADERS_INSTALL_DIR="$HDR_INSTALL" \
  -DPython3_EXECUTABLE="$(command -v python3)" \
  -DBUILD_TESTS=OFF \
  -DBUILD_WERROR=OFF \
  -DENABLE_WERROR=OFF \
  -DBUILD_WSI_XCB_SUPPORT=OFF \
  -DBUILD_WSI_XLIB_SUPPORT=OFF \
  -DBUILD_WSI_WAYLAND_SUPPORT=OFF \
  -DBUILD_WSI_DIRECTFB_SUPPORT=OFF \
  -DCMAKE_C_FLAGS="-Wno-typedef-redefinition" \
  >/dev/null

echo "[+] build"
cmake --build "$BUILD/loader" -j"$(nproc)" >/dev/null

# --- 3. stage the real libvulkan.so.1 (dereference the version symlink) ----
SO_REAL="$(readlink -f "$BUILD/loader/loader/libvulkan.so.1")"
[ -f "$SO_REAL" ] || { echo "error: loader build produced no libvulkan.so.1" >&2; exit 1; }
mkdir -p "$OUT"
cp -f "$SO_REAL" "$OUT/libvulkan.so.1"
"$NDKBIN/llvm-strip" --strip-unneeded "$OUT/libvulkan.so.1"

# --- 4. verify SONAME + Bionic-cleanliness + generic discovery -------------
RE="$NDKBIN/llvm-readelf"
soname="$("$RE" -d "$OUT/libvulkan.so.1" | awk -F'[][]' '/SONAME/{print $2}')"
needed="$("$RE" -d "$OUT/libvulkan.so.1" | awk -F'[][]' '/NEEDED/{print $2}' | tr '\n' ' ')"
badver="$("$RE" -V "$OUT/libvulkan.so.1" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+|ZLIB_[0-9.]+' | sort -u | tr '\n' ' ' || true)"
# grep -c exits 1 on zero matches; || true so set -e doesn't kill us (0 HAL
# strings is the GOOD outcome, and we only want the count, not grep's status).
hal="$(strings "$OUT/libvulkan.so.1" | grep -cE 'hw_get_module|vulkan\.%s|hwvulkan' || true)"
icd="$(strings "$OUT/libvulkan.so.1" | grep -c 'VK_ICD_FILENAMES' || true)"

echo
echo "[=] $OUT/libvulkan.so.1 ($(du -h "$OUT/libvulkan.so.1" | cut -f1))"
echo "    SONAME : $soname"
echo "    NEEDED : $needed"
echo "    VK_ICD_FILENAMES discovery: $icd   Android-HAL strings: $hal"
fail=0
[ "$soname" = "libvulkan.so.1" ]                 || { echo "FAIL: SONAME != libvulkan.so.1" >&2; fail=1; }
[ -z "$badver" ]                                 || { echo "FAIL: non-Bionic verneeds: $badver" >&2; fail=1; }
[ "$icd" -ge 1 ]                                 || { echo "FAIL: no VK_ICD_FILENAMES discovery (not the generic loader)" >&2; fail=1; }
[ "$hal" -eq 0 ]                                 || { echo "FAIL: Android-HAL strings present (wrong discovery path)" >&2; fail=1; }
[ "$fail" -eq 0 ] || exit 1
echo "[=] OK — source Khronos loader staged for turnip-libs.tar.gz"
