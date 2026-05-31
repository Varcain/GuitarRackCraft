#!/usr/bin/env bash
# Cross-compile wine for Android Bionic arm64 using NDK r26+.
#
# This produces wine's Unix-side binaries (loader/wine, server/wineserver,
# libwine.so) as arm64-linux-android ELF binaries linked against Bionic libc.
# These complement the PE-side ARM64X DLLs from scripts/build-wine-pe.sh.
#
# First-time runs will reveal Bionic adaptation issues; document each in
# docs/fex-pivot-bionic-patches.md and put fixes in patches/wine/.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

# --- apply Bionic adaptation patches to clean upstream wine -----------------
# wine-upstream is a submodule pinned at the wine-10.10 tag (clean upstream).
# Our Bionic / FEX-pivot adaptations live as files in patches/wine/ and are
# applied here before configure. Re-running this script resets wine and
# re-applies — see apply-wine-patches.sh for details.
"$(dirname "$0")/apply-wine-patches.sh"

# --- toolchain --------------------------------------------------------------
NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/26.1.10909125}"
[ -d "$NDK" ] || { echo "error: NDK not found at $NDK; export ANDROID_NDK"; exit 1; }
HOST_TAG=linux-x86_64
TARGET=aarch64-linux-android
API=28
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
export PATH="$TOOLCHAIN/bin:$PATH"
# also keep llvm-mingw on PATH so the PE-side build still works from this
# same shell environment (mingw triple won't collide with NDK clang).
export PATH="$repo_root/external/llvm-mingw/install/bin:$PATH"

export CC="$TOOLCHAIN/bin/${TARGET}${API}-clang"
export CXX="$TOOLCHAIN/bin/${TARGET}${API}-clang++"
export AR="$TOOLCHAIN/bin/llvm-ar"
export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
export STRIP="$TOOLCHAIN/bin/llvm-strip"

# --- build dir --------------------------------------------------------------
build_dir="external/wine-upstream/build-android-arm64"
mkdir -p "$build_dir"
cd "$build_dir"

if [ ! -f Makefile ]; then
  echo "=== configure wine for Android Bionic arm64 ==="
  # wine 11.9 hard-requires gradle in linux-android* case even when
  # wineandroid_drv is disabled. Spoof with /bin/true since we don't
  # invoke gradle (wineandroid.drv build is disabled below).
  export GRADLE=/bin/true
  # FreeType lives outside the X11 include sweep but in the same staged
  # tree (toolchain/x11-headers contains ft2build.h + freetype/). Wine's
  # configure does its own AC_CHECK_HEADER([ft2build.h]) — we need it on
  # CPPFLAGS or that probe fails and configure exits.
  #
  # Add NDK sysroot's EGL/GLES headers/libs so wine's configure can detect
  # libEGL.so via WINE_CHECK_SONAME — without this, win32u/opengl.c falls
  # back to its stub egl_init() that just WARNs "EGL support not compiled
  # in!" at runtime, breaking all D3D/GL plugins (AmpCraft, etc.).
  EGL_LIB_DIR="$TOOLCHAIN/sysroot/usr/lib/aarch64-linux-android/$API"
  export CPPFLAGS="-I$repo_root/toolchain/x11-headers"
  export LDFLAGS="-L$repo_root/toolchain/x11-libs -L$EGL_LIB_DIR -L$repo_root/toolchain/gnutls-android-arm64/lib"
  # --with-wine-tools points at our existing x86_64 Linux host build, which
  # has widl / winegcc / etc. already compiled. Cross-compile builds reuse
  # those rather than running the wine tools through the cross compiler.
  #
  # --disable-wineandroid_drv: skip the upstream wineandroid.drv display
  # driver. Its build runs `gradle assembleDebug` against a 2016-era
  # build.gradle.in (Android Gradle Plugin 2.2.1 + jcenter), which doesn't
  # work with modern toolchains. We don't need it: our app provides its
  # own Compose Activity and wine talks to it via the in-process X server
  # we wrote on master.
  #
  # X11 support uses Termux's prebuilt arm64 libs (Bionic-compatible —
  # they link against libc.so / libandroid-support.so / libdl.so, not
  # glibc). Headers from /usr/include/X11 copied into toolchain/
  # x11-headers/ (arch-agnostic). Libs from .deb packages staged under
  # toolchain/x11-libs/. Re-stage with scripts/fetch-x11-libs.sh.
  # --disable-win16: see comment in build-wine-pe.sh — clang 21.1-rc2 crashes
  # on dlls/krnl386.exe16/selector.c (16-bit inline asm) and we don't need
  # Win16 anyway (FEX-Emu doesn't translate it).
  ../configure \
    --host="$TARGET" \
    --with-wine-tools=../build-arm64ec \
    --enable-archs=arm64ec,aarch64,i386 \
    --with-mingw=clang \
    --disable-win16 \
    --disable-tests \
    --x-includes="$repo_root/toolchain/x11-headers" \
    --x-libraries="$repo_root/toolchain/x11-libs" \
    --with-freetype \
    --without-alsa \
    --without-pulse \
    --without-fontconfig \
    --without-gstreamer \
    --without-cups \
    --without-dbus \
    GNUTLS_CFLAGS="-I$repo_root/toolchain/gnutls-android-arm64/include" \
    GNUTLS_LIBS="-L$repo_root/toolchain/gnutls-android-arm64/lib -lgnutls" \
    --with-gnutls \
    --without-krb5 \
    --without-sane \
    --without-usb \
    --without-pcap \
    --without-pcsclite \
    --without-udev \
    --without-v4l2 \
    --without-xshm \
    || { echo "configure FAILED — see config.log"; exit 1; }
  # XShm uses SysV shmget(syscall 194) which Android's untrusted_app seccomp
  # blocks → SIGSYS kills the wine process the moment winex11.drv tries to
  # allocate a shared-memory pixmap (observed during plugin effEditOpen on
  # WagnerSharp.dll). Wine's bitblt.c falls back to XPutImage without SHM
  # when HAVE_LIBXXSHM is undefined.
fi

echo "=== make (this will take a while) ==="
make -j"$(nproc)"
