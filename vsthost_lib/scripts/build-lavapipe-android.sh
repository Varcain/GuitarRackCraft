#!/usr/bin/env bash
# Build Mesa lavapipe (software Vulkan ICD: libvulkan_lvp.so + lvp_icd.json) for
# Android Bionic arm64, from the in-tree 3rd_party/mesa submodule, linked against
# the android-arm64 libLLVM built by build-llvm-android.sh. This is the UNIVERSAL
# software-Vulkan FALLBACK: when no GPU Vulkan driver can run zink/DXVK (Turnip on
# Adreno, the vendor Vulkan elsewhere), the shim (vulkan_turnip_shim.c) loads
# lavapipe so the editor still renders — slow (CPU) but correct, with every Vulkan
# feature (incl. robustness2/nullDescriptor that DXVK needs and Qualcomm lacks).
#
# Sibling of build-turnip-icd.sh; differs by: vulkan-drivers=swrast + llvmpipe +
# llvm=enabled, linking the cross-built LLVM via the cmake method (a TARGET
# llvm-config can't run on the host, so we disable llvm-config and point cmake at
# the LLVM install with CMAKE_PREFIX_PATH).
#
# Run after build-llvm-android.sh. Called by build-all.sh (phase "lavapipe").
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"            # vsthost_lib/
top="$(cd "$repo_root/.." && pwd)"                       # GuitarRackCraft/
M="$top/3rd_party/mesa"
PATCHES="$repo_root/patches/mesa"
STUBS="$PATCHES/build-files/android-deps-include"        # cutils/*.h, log/log.h
DRM_SYSROOT="$repo_root/toolchain/drm-android"           # source libdrm (build-libdrm-android.sh)
LLVM_PREFIX="$repo_root/external/llvm-android/install-android-arm64"
# Stage into turnip-libs/ so lavapipe rides the existing turnip-libs.tar.gz and
# lands in <wine>/turnip/ at runtime — next to the source Khronos loader
# (libvulkan.so.1), which loads it via VK_ICD_FILENAMES=<turnip>/lvp_icd.json.
OUT="$repo_root/toolchain/turnip-libs"
PREFIX="$M/build-android-lavapipe/install"

NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/26.1.10909125}"
NDKBIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
API=28

# mesa 24.2.8 finds LLVM via the cmake method when cross-compiling (no runnable
# target llvm-config). meson < 1.4's cmake LLVM probe tops out at LLVM-17 and
# crashes on an 'unknown version' sentinel, so it can't see our LLVM 18. Prefer an
# isolated venv meson (>= 1.4) so we don't have to upgrade the system meson the
# turnip/zink/wine builds rely on. Create it with:
#   python3 -m venv vsthost_lib/.cache/meson-venv
#   vsthost_lib/.cache/meson-venv/bin/pip install 'meson==1.7.2'
VENV_MESON="$repo_root/.cache/meson-venv/bin/meson"
MESON="${LAVAPIPE_MESON:-$([ -x "$VENV_MESON" ] && echo "$VENV_MESON" || echo meson)}"

[ -d "$M/.git" ] || [ -f "$M/.git" ] || { echo "error: mesa submodule not at $M" >&2; exit 1; }
[ -f "$LLVM_PREFIX/lib/cmake/llvm/LLVMConfig.cmake" ] || {
    echo "error: android-arm64 LLVM not built ($LLVM_PREFIX) — run build-llvm-android.sh first" >&2; exit 1; }
# mesa requires libdrm whenever system_has_kms_drm + with_dri2 (default on Linux),
# even for the software llvmpipe/lavapipe build. lavapipe never opens a DRM device,
# so this is just a harmless NEEDED libdrm.so (already shipped in turnip-libs/).
[ -f "$DRM_SYSROOT/lib/pkgconfig/libdrm.pc" ] || {
    echo "error: source libdrm not at $DRM_SYSROOT — run build-libdrm-android.sh first" >&2; exit 1; }
[ -x "$NDKBIN/aarch64-linux-android$API-clang" ] || { echo "error: NDK not at $NDK" >&2; exit 1; }
command -v ninja >/dev/null || { echo "error: ninja not on PATH" >&2; exit 1; }
_mver="$("$MESON" --version 2>/dev/null || echo 0)"
if [ "$(printf '%s\n1.4.0\n' "$_mver" | sort -V | head -1)" != "1.4.0" ]; then
    echo "error: meson $_mver ($MESON) is too old for LLVM 18 via the cmake method (need >= 1.4)." >&2
    echo "       fix: python3 -m venv $repo_root/.cache/meson-venv && \\" >&2
    echo "            $repo_root/.cache/meson-venv/bin/pip install 'meson==1.7.2'" >&2
    exit 1
fi

echo "=== Mesa lavapipe (software Vulkan, libvulkan_lvp.so) + LLVM 18 (android-arm64) ==="
echo "    mesa: $(git -C "$M" describe --tags 2>/dev/null || echo '?')   LLVM: $LLVM_PREFIX"
echo "    meson: $_mver ($MESON)"

echo "[+] reset mesa submodule (preserve build dirs)"
git -C "$M" reset --hard HEAD >/dev/null
git -C "$M" clean -fdx -e build-android-zink -e build-android-turnip -e build-android-turnip-hal \
    -e build-android-lavapipe -e .android-deps >/dev/null

# MESA_FORCE_LINUX (detect_os hatch, as in build-turnip-icd) → generic Linux ICD,
# no Android gralloc/AHB code; lavapipe is offscreen software so it needs no WSI.
echo "[+] apply patch 0002 (MESA_FORCE_LINUX hatch + drop VK_KHR_display WSI on Bionic)"
git -C "$M" apply "$PATCHES/0002-turnip-icd-bionic-no-display-wsi.patch"

# llvmpipe's DETECT_OS_LINUX device-memory path backs allocations with a shared
# memfd (mmap MAP_SHARED), which an Android untrusted_app's seccomp blocks ->
# map_memory returns MAP_FAILED -> DXVK's first vkMapMemory writes to -1 -> crash.
# Fall back to plain malloc on Android (MESA_FORCE_LINUX). Needed for DXVK/D3D11
# plugins (AmpliTube, Ampbox) on software lavapipe; lavapipe presents via readback
# so the shared-fd is unnecessary.
echo "[+] apply patch 0004 (llvmpipe malloc device-memory on Android)"
git -C "$M" apply "$PATCHES/0004-llvmpipe-malloc-device-memory-on-android.patch"

# mesa hardcodes the LLVM dependency to method:'config-tool' on non-Windows, which
# needs a runnable target llvm-config (impossible when cross-compiling: a target
# aarch64 llvm-config can't run on the x86 host). Flip it to 'auto' so meson tries
# config-tool (we force-fail it via llvm-config='false') then falls to the cmake
# method, which reads our cross-built install's LLVMConfig.cmake (it handles the
# transitive module→.a expansion natively). One surgical line; reverted at the end.
echo "[+] patch mesa LLVM dependency method config-tool → auto (use cmake find_package)"
sed -i "s/method : host_machine.system() == 'windows' ? 'auto' : 'config-tool',/method : 'auto',/" \
    "$M/meson.build"
grep -q "method : 'auto'," "$M/meson.build" || { echo "error: LLVM method sed did not apply" >&2; exit 1; }

CROSS="$M/android-lavapipe-aarch64.ini"
cat > "$CROSS" <<EOF
# AUTO-GENERATED by build-lavapipe-android.sh — machine-local paths. Do not commit.
[binaries]
c = '$NDKBIN/aarch64-linux-android$API-clang'
cpp = '$NDKBIN/aarch64-linux-android$API-clang++'
ar = '$NDKBIN/llvm-ar'
strip = '$NDKBIN/llvm-strip'
c_ld = 'lld'
cpp_ld = 'lld'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=$DRM_SYSROOT/lib/pkgconfig', 'pkg-config']
# cmake must be listed for cross builds or meson reports "Found CMake: NO" and
# skips the cmake dependency method entirely.
cmake = '$(command -v cmake)'
# Disable llvm-config so meson uses the cmake method (the target llvm-config can't
# run on the host — and an unset llvm-config would resolve the HOST x86 LLVM);
# cmake then finds the TARGET LLVMConfig.cmake via cmake_prefix_path / CMAKE_PREFIX_PATH.
llvm-config = 'false'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true
cmake_prefix_path = ['$LLVM_PREFIX']

# meson generates its CMake cross-toolchain's CMAKE_SYSTEM_NAME from
# [host_machine] system (= 'android'), which makes CMake enter Android mode and
# demand CMAKE_ANDROID_NDK/ABI it wasn't given → "Failed to determine CMake
# compilers state" → the cmake dependency method is disabled, and LLVM (which mesa
# only finds via cmake when cross-compiling) is never found. Override JUST the
# CMake system name to Linux: the NDK clang already targets aarch64-linux-android
# (fixed triple), so as a generic Linux cross-compiler the compiler-id test passes,
# and find_package(LLVM) reads our LLVMConfig.cmake via cmake_prefix_path with no
# Android FIND_ROOT_PATH=ONLY restriction. mesa still sees system()=='android'.
[cmake]
CMAKE_SYSTEM_NAME = 'Linux'
CMAKE_SYSTEM_PROCESSOR = 'aarch64'

[built-in options]
c_args = ['-I$STUBS', '-include', '$PATCHES/build-files/memfd_compat.h', '-D__USE_GNU', '-DMESA_FORCE_LINUX', '-DLVP_USE_WSI_PLATFORM']
cpp_args = ['-I$STUBS', '-include', '$PATCHES/build-files/memfd_compat.h', '-D__USE_GNU', '-DMESA_FORCE_LINUX', '-DLVP_USE_WSI_PLATFORM']
c_link_args = ['-static-libstdc++', '-llog']
cpp_link_args = ['-static-libstdc++', '-llog']
EOF

B="$M/build-android-lavapipe"
echo "[+] meson setup (vulkan-drivers=swrast=lavapipe, gallium=llvmpipe, llvm static)"
rm -rf "$B"
# --force-fallback-for=zlib,expat: our pkg-config is scoped to the drm sysroot (no
# zlib/expat there), so with the cmake method active (for LLVM) meson would
# otherwise find the HOST x86_64 zlib/expat via find_package — dragging
# /usr/include + host .so into every target compile (the pthread regparm error).
# Build them from mesa's bundled wraps for the target instead.
CMAKE_PREFIX_PATH="$LLVM_PREFIX" \
"$MESON" setup "$B" "$M" --cross-file "$CROSS" \
  --buildtype=release --default-library=shared --prefix="$PREFIX" \
  --force-fallback-for=zlib,expat \
  -Dvulkan-drivers=swrast -Dgallium-drivers=llvmpipe \
  -Dllvm=enabled -Dshared-llvm=disabled -Dcpp_rtti=false \
  -Dplatforms= -Degl=disabled -Dgbm=disabled -Dglx=disabled \
  -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
  -Ddri3=disabled -Dshared-glapi=disabled -Dxmlconfig=disabled \
  -Dvulkan-layers= -Dvalgrind=disabled -Dlibunwind=disabled \
  -Dandroid-stub=false -Dzstd=disabled -Db_ndebug=true

echo "[+] ninja install"
ninja -C "$B"
rm -rf "$PREFIX"; ninja -C "$B" install

# --- stage the lavapipe ICD + manifest --------------------------------------
ICD_SO="$PREFIX/lib/libvulkan_lvp.so"
ICD_JSON="$(ls "$PREFIX"/share/vulkan/icd.d/lvp_icd*.json 2>/dev/null | head -1)"
[ -f "$ICD_SO" ]   || { echo "error: build produced no libvulkan_lvp.so" >&2; exit 1; }
[ -f "$ICD_JSON" ] || { echo "error: build produced no lvp_icd json" >&2; exit 1; }
mkdir -p "$OUT"
cp -f "$ICD_SO" "$OUT/libvulkan_lvp.so"
"$NDKBIN/llvm-strip" --strip-unneeded "$OUT/libvulkan_lvp.so"
cp -f "$ICD_JSON" "$OUT/lvp_icd.aarch64.json"
python3 - "$OUT/lvp_icd.aarch64.json" <<'PY'
import json,sys
p=sys.argv[1]; d=json.load(open(p)); d["ICD"]["library_path"]="libvulkan_lvp.so"
json.dump(d,open(p,"w"),indent=4)
PY

RE="$NDKBIN/llvm-readelf"; F="$OUT/libvulkan_lvp.so"
echo; echo "[=] $F ($(du -h "$F" | cut -f1))"
echo "    vk_icdGetInstanceProcAddr: $("$RE" --dyn-syms "$F" | grep -c vk_icdGetInstanceProcAddr || true)"
echo "    NEEDED: $("$RE" -d "$F" | awk -F'[][]' '/NEEDED/{print $2}' | tr '\n' ' ')"
git -C "$M" checkout -- meson.build src/util/detect_os.h src/vulkan/meson.build src/vulkan/wsi/meson.build 2>/dev/null || true
echo "[=] OK — lavapipe staged for the software-Vulkan fallback asset"
