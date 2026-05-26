#!/usr/bin/env bash
# Cross-compile libpng + freetype for Android Bionic (arm64) using the
# NDK clang toolchain. Produces clean shared libs with no glibc-style
# symbol versioning — drop-in replacements for the Termux ones that
# Bionic's linker rejected ("cannot find 'Export' from verneed[0]",
# "cannot locate symbol 'inflateInit2_'", etc).
#
# Outputs:
#   toolchain/android-libs/lib/libpng16.so      (NEEDED: libz.so)
#   toolchain/android-libs/lib/libfreetype.so   (NEEDED: libpng16.so libz.so)
#   toolchain/android-libs/include/...          (headers wine's configure needs)
#
# Wine consumes those via toolchain/x11-{libs,headers}/ (the same staging
# directory the X11 build uses). After this script: regenerate jniLibs
# with scripts/pack-wine-fex.py.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/26.1.10909125}"
[ -d "$NDK" ] || { echo "error: NDK not found at $NDK"; exit 1; }
HOST_TAG=linux-x86_64
TARGET=aarch64-linux-android
API=28
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"

export CC="$TOOLCHAIN/bin/${TARGET}${API}-clang"
export AR="$TOOLCHAIN/bin/llvm-ar"
export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
export STRIP="$TOOLCHAIN/bin/llvm-strip"

cache=.cache/sources
install_root="$repo_root/toolchain/android-libs"
mkdir -p "$install_root" "$cache"

fetch_tarball() {
    local name="$1" url="$2"
    if [ ! -f "$cache/$name" ]; then
        echo "[+] fetch $name"
        curl -fSL --retry 3 -o "$cache/$name" "$url"
    fi
}

# --- libpng ----------------------------------------------------------------
png_ver=1.6.43
fetch_tarball "libpng-${png_ver}.tar.xz" \
    "https://download.sourceforge.net/libpng/libpng-${png_ver}.tar.xz"
png_src="$cache/libpng-${png_ver}"
if [ ! -d "$png_src" ]; then
    tar xJf "$cache/libpng-${png_ver}.tar.xz" -C "$cache"
fi
echo "=== build libpng-${png_ver} ==="
pushd "$png_src" >/dev/null
make clean 2>/dev/null || true
./configure --host="$TARGET" \
    --prefix="$install_root" \
    --disable-static \
    --without-libpng-compat \
    CFLAGS="-O2 -fPIC" \
    LDFLAGS="-Wl,--no-undefined" >/dev/null
make -j"$(nproc)" install >/dev/null
popd >/dev/null

# --- freetype --------------------------------------------------------------
ft_ver=2.13.3
fetch_tarball "freetype-${ft_ver}.tar.xz" \
    "https://download.savannah.gnu.org/releases/freetype/freetype-${ft_ver}.tar.xz"
ft_src="$cache/freetype-${ft_ver}"
if [ ! -d "$ft_src" ]; then
    tar xJf "$cache/freetype-${ft_ver}.tar.xz" -C "$cache"
fi
echo "=== build freetype-${ft_ver} ==="
pushd "$ft_src" >/dev/null
make clean 2>/dev/null || true
PKG_CONFIG_LIBDIR="$install_root/lib/pkgconfig" \
LIBPNG_CFLAGS="-I$install_root/include/libpng16" \
LIBPNG_LIBS="-L$install_root/lib -lpng16" \
ZLIB_CFLAGS="" \
ZLIB_LIBS="-lz" \
./configure --host="$TARGET" \
    --prefix="$install_root" \
    --disable-static \
    --without-bzip2 \
    --without-brotli \
    --without-harfbuzz \
    --without-fsref \
    --without-quickdraw-toolbox \
    --without-quickdraw-carbon \
    --without-ats \
    CFLAGS="-O2 -fPIC" \
    LDFLAGS="-Wl,--no-undefined -L$install_root/lib" >/dev/null
make -j"$(nproc)" install >/dev/null
popd >/dev/null

echo "=== stage into toolchain/x11-{libs,headers} ==="
# Stage so the wine build picks them up via its existing --x-includes /
# --x-libraries paths.
cp -f "$install_root/lib/libpng16.so" "$repo_root/toolchain/x11-libs/"
cp -f "$install_root/lib/libfreetype.so" "$repo_root/toolchain/x11-libs/"
# Headers — wine's configure looks for ft2build.h + freetype/freetype.h
# directly under the include path.
cp -rf "$install_root/include/freetype2/." "$repo_root/toolchain/x11-headers/"
# libpng16 headers
mkdir -p "$repo_root/toolchain/x11-headers/libpng16"
cp -f "$install_root/include/libpng16"/*.h "$repo_root/toolchain/x11-headers/libpng16/" 2>/dev/null || true

# Drop libs we no longer need (Termux freetype chain).
rm -f "$repo_root/toolchain/x11-libs"/libbz2.so \
       "$repo_root/toolchain/x11-libs"/libbrotlidec.so \
       "$repo_root/toolchain/x11-libs"/libbrotlicommon.so \
       "$repo_root/toolchain/x11-libs"/libz.so

echo
echo "next:"
echo "  - rerun scripts/build-wine-android.sh (configure will detect new libs)"
echo "  - scripts/pack-wine-fex.py to refresh jniLibs"
