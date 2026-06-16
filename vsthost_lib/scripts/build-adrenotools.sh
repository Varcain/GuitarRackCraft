#!/usr/bin/env bash
# Builds libadrenotools + its 4 namespace-bypass hook libs (bylaws/libadrenotools)
# for arm64 Bionic with the NDK, and stages the 5 .so into toolchain/adrenotools-libs/.
#
# WHY: Turnip is loaded as an ANDROID-HAL GPU driver (vulkan.ad07xx.so) via
# libadrenotools — a Winlator-style linker-namespace hook that gives the driver
# /dev/kgsl access on untrusted_app (renderD128/DRM is SELinux-denied to us).
# win32u/vulkan.c and the mesa vkshim dlopen libadrenotools.so + call
# adrenotools_open_libvulkan(); adrenotools in turn loads its hook libs
# (libmain_hook/libhook_impl/libfile_redirect_hook/libgsl_alloc_hook) by soname
# from VSTPOC_ADRENOTOOLS_HOOKDIR (= the APK nativeLibraryDir). All 5 must ship
# under their REAL names. pack-wine-fex.py copies toolchain/adrenotools-libs/*
# into src/main/jniLibs/arm64-v8a/. Without them the adrenotools path can't load
# → AmpliTube's (and any Mesa-Zink) GL editor renders black. These were
# previously built+copied by hand (lost on a fresh clone / CI). See
# feedback_amplitube_turnip_driver_name_regression.
#
# gen/bcenabler_patch.h is pre-generated in the submodule, so build_asm.sh is
# NOT needed (it only regenerates that header, and would need a host
# aarch64 cross-gcc + bin2c).

set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/26.1.10909125}"
[ -d "$NDK" ] || { echo "error: NDK not found at $NDK; export ANDROID_NDK" >&2; exit 1; }

src="external/libadrenotools"
if [ ! -f "$src/CMakeLists.txt" ] || [ ! -f "$src/lib/linkernsbypass/CMakeLists.txt" ]; then
  echo "error: $src not initialized — run from the GuitarRackCraft root:" >&2
  echo "  git submodule update --init --recursive vsthost_lib/external/libadrenotools" >&2
  exit 1
fi

build="$src/build-android-arm64"
out="toolchain/adrenotools-libs"
mkdir -p "$out"

# Default (Make) generator — no Ninja dependency. The NDK toolchain file + the
# committed gen/ header are all that's needed.
if [ ! -f "$build/CMakeCache.txt" ]; then
  cmake -B "$build" -S "$src" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-28 \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "$build" --parallel "$(nproc)"

# Stage the main lib + 4 hooks under their real sonames.
for lib in libadrenotools.so libhook_impl.so libmain_hook.so \
           libfile_redirect_hook.so libgsl_alloc_hook.so; do
  f="$(find "$build" -name "$lib" -type f | head -1)"
  [ -n "$f" ] || { echo "error: built lib $lib not found under $build" >&2; exit 1; }
  cp -f "$f" "$out/$lib"
done

echo "[+] staged $(ls "$out"/*.so 2>/dev/null | wc -l) adrenotools libs in $out/"
ls -la "$out"/*.so 2>/dev/null | awk '{print "    "$5"  "$NF}'

# Sanity: the entry point the wine/vkshim side dlsyms must be exported.
# (Read into a var first — piping readelf into grep under `set -o pipefail`
# would report a spurious failure on readelf's own non-zero exit.)
if command -v llvm-readelf >/dev/null 2>&1; then
  syms="$(llvm-readelf --dyn-syms "$out/libadrenotools.so" 2>/dev/null || true)"
  case "$syms" in
    *adrenotools_open_libvulkan*) ;;
    *) echo "WARN: libadrenotools.so does not export adrenotools_open_libvulkan" >&2 ;;
  esac
fi
