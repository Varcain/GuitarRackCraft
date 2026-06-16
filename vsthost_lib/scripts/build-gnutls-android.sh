#!/usr/bin/env bash
# Build gnutls + dependencies for arm64-android, install into toolchain/gnutls-android-arm64.
# This is what wine's secur32 needs for Schannel/TLS support — without it, all HTTPS
# requests from plugins fail (TH-U Manage License, demo mode, license servers, etc).

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/26.1.10909125}"
[ -d "$NDK" ] || { echo "error: NDK not found at $NDK"; exit 1; }
HOST_TAG=linux-x86_64
TARGET=aarch64-linux-android
API=28
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
export PATH="$TOOLCHAIN/bin:$PATH"

PREFIX="$repo_root/toolchain/gnutls-android-arm64"
mkdir -p "$PREFIX"

export CC="$TOOLCHAIN/bin/${TARGET}${API}-clang"
export CXX="$TOOLCHAIN/bin/${TARGET}${API}-clang++"
export AR="$TOOLCHAIN/bin/llvm-ar"
export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
export STRIP="$TOOLCHAIN/bin/llvm-strip"
export CPPFLAGS="-I$PREFIX/include"
export LDFLAGS="-L$PREFIX/lib"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
# NDK r26's clang-17 promotes -Wimplicit-function-declaration to an ERROR.
# gnutls's tools-only gnulib (src/gl/libgnu_gpl, built even with --disable-tools)
# calls gnulib time helpers (tzalloc/mktime_z/localtime_rz/tzfree) that Bionic
# lacks and whose gnulib replacements aren't declared on Android → hard error.
# That convenience lib is NOT linked into libgnutls.so (lib/ uses its own
# gnulib), so downgrade the error to a warning to let the build complete. Only
# ever a fresh-build issue — cached builds skip it via the .built.* sentinels.
export CFLAGS="-Wno-error=implicit-function-declaration"

deps_dir="$repo_root/external/gnutls-deps"
mkdir -p "$deps_dir"
cd "$deps_dir"

# Fetch upstream tarballs if not already cached. Versions pinned for
# reproducibility; bump when upstream releases.
fetch_tarball() {
    local name="$1" url="$2"
    if [ ! -f "$name" ]; then
        echo "[+] fetch $name"
        # Download to a temp file + atomic rename: an interrupted transfer
        # (curl 18 "partial file" — seen on CI) must never leave a corrupt
        # cached tarball that the [ ! -f ] guard would then trust on re-run.
        # --retry-all-errors retries partial transfers, which plain --retry does not.
        curl -fSL --retry 5 --retry-delay 2 --retry-all-errors \
             --connect-timeout 30 -o "$name.part" "$url"
        mv -f "$name.part" "$name"
    fi
}
# gmp from ftp.gnu.org (GNU mirror), not gmplib.org — the latter intermittently
# times out / blocks CI runners (curl 28). Same host as the other deps below.
fetch_tarball gmp-6.3.0.tar.xz         "https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz"
fetch_tarball nettle-3.10.tar.gz       "https://ftp.gnu.org/gnu/nettle/nettle-3.10.tar.gz"
fetch_tarball libtasn1-4.20.0.tar.gz   "https://ftp.gnu.org/gnu/libtasn1/libtasn1-4.20.0.tar.gz"
fetch_tarball libunistring-1.2.tar.xz  "https://ftp.gnu.org/gnu/libunistring/libunistring-1.2.tar.xz"
fetch_tarball gnutls-3.8.6.tar.xz      "https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/gnutls-3.8.6.tar.xz"

build_pkg() {
    local archive=$1; local dir=$2; local extra_conf="${3:-}"
    if [ -f "$PREFIX/.built.$dir" ]; then echo "=== $dir already built ==="; return; fi
    echo "=== building $dir ==="
    rm -rf "$dir"
    case "$archive" in
        *.tar.xz) tar xf "$archive" ;;
        *.tar.gz) tar xzf "$archive" ;;
    esac
    cd "$dir"
    # Build deps as STATIC only — they'll be embedded into libgnutls.so so
    # we don't have to deal with multi-lib SONAME/jniLibs naming on Android.
    ./configure --host=aarch64-linux-android --prefix="$PREFIX" \
        --disable-shared --enable-static --disable-doc \
        --with-pic \
        $extra_conf
    make -j"$(nproc)"
    make install
    touch "$PREFIX/.built.$dir"
    cd ..
}

# 1. gmp (needed by nettle)
build_pkg gmp-6.3.0.tar.xz gmp-6.3.0

# 2. nettle (needs gmp)
build_pkg nettle-3.10.tar.gz nettle-3.10 "--disable-openssl --disable-assembler"

# 3. libtasn1 (standalone)
build_pkg libtasn1-4.20.0.tar.gz libtasn1-4.20.0

# 4. libunistring (standalone)
build_pkg libunistring-1.2.tar.xz libunistring-1.2

# 5. gnutls (needs all of the above; skip optional deps)
if [ ! -f "$PREFIX/.built.gnutls" ]; then
    echo "=== building gnutls-3.8.6 ==="
    rm -rf gnutls-3.8.6
    tar xf gnutls-3.8.6.tar.xz
    cd gnutls-3.8.6
    # gl_cv_func_have_ld_version_script=no disables the --version-script linker
    # arg that produces glibc-style symbol versioning. Bionic doesn't speak
    # that dialect and refuses to load libs with verneed entries for "LIBC"
    # versions. Without versioning, gnutls falls back to -export-symbols-regex
    # which works fine for our dlopen-only use case.
    # Build libgnutls.so as SHARED with all static deps embedded.
    # gl_cv_func_have_ld_version_script=no disables glibc-style symbol
    # versioning that Bionic can't grok.
    # -Wl,-Bsymbolic ensures internal symbol references resolve to the
    # embedded static copies instead of looking for external libs at
    # runtime.
    gl_cv_func_have_ld_version_script=no \
    ac_cv_func_strverscmp=no \
    gl_cv_func_strverscmp=no \
    CPPFLAGS="-D_GNU_SOURCE" \
    ./configure --host=aarch64-linux-android --prefix="$PREFIX" \
        --enable-shared --enable-static --disable-doc --disable-tests --disable-tools \
        --without-p11-kit --disable-libdane --disable-cxx --disable-guile \
        --without-tpm --without-tpm2 \
        --without-zlib --without-brotli --without-zstd \
        --without-idn \
        --disable-nls --with-pic \
        --with-included-libtasn1=no --with-included-unistring=no \
        --disable-doc \
        GMP_LIBS="-Wl,-Bstatic -lgmp -Wl,-Bdynamic" GMP_CFLAGS="-I$PREFIX/include" \
        NETTLE_LIBS="-Wl,-Bstatic -lhogweed -lnettle -lgmp -Wl,-Bdynamic" NETTLE_CFLAGS="-I$PREFIX/include" \
        LIBS="-Wl,-Bstatic -lunistring -ltasn1 -Wl,-Bdynamic" \
        LDFLAGS="-L$PREFIX/lib -Wl,-Bsymbolic"
    make -j"$(nproc)"
    make install
    touch "$PREFIX/.built.gnutls"
    cd ..
fi

echo ""
echo "=== gnutls install summary ==="
ls -la "$PREFIX/lib/" | grep -E "libgnutls|libnettle|libgmp|libtasn1|libunistring|libhogweed"
echo ""
echo "=== gnutls headers ==="
ls "$PREFIX/include/" | head
