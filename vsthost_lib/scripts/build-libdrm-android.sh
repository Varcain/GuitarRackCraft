#!/usr/bin/env bash
# Build libdrm from source for arm64 Android Bionic (NDK + meson), freedreno
# backend only. Replaces the Termux libdrm prebuilt — both the .deb fetched by
# fetch-turnip-libs.sh (libdrm.so, the Turnip ICD's only real runtime dep on the
# msm/DRM path) AND the checked-in Termux blob at
# patches/mesa/build-files/android-deps-data/lib/ that build-mesa-zink.sh links.
# Phase 2 of the prebuilt→source migration.
#
# mesa 24.2.8 requires libdrm >= 2.4.109 (freedreno needs only the base min).
# We build ONLY freedreno (+ its Android kgsl API) and disable every other GPU
# backend, udev, tests, and the cairo/valgrind extras → libdrm.so + libdrm_freedreno.so
# linking only libc. Output sysroot: toolchain/drm-android/{lib,include,lib/pkgconfig}
# (the Turnip ICD build + build-mesa-zink point pkg-config there; the runtime .so
# is staged into toolchain/turnip-libs/ for the asset).
#
# Called by scripts/build-all.sh (phase "turnip", before build-turnip-icd.sh);
# also runnable standalone.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"            # vsthost_lib/
cd "$repo_root"

NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/26.1.10909125}"
NDKBIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
API=28
DRM_VER="${DRM_VER:-2.4.125}"

cache="$repo_root/.cache/sources"
SYSROOT="$repo_root/toolchain/drm-android"               # install prefix (build-time sysroot)
TURNIP_OUT="$repo_root/toolchain/turnip-libs"            # runtime staging (asset)

[ -x "$NDKBIN/aarch64-linux-android$API-clang" ] || { echo "error: NDK not at $NDK (set ANDROID_NDK)" >&2; exit 1; }
for t in meson ninja; do command -v "$t" >/dev/null || { echo "error: $t not on PATH" >&2; exit 1; }; done

mkdir -p "$cache"
tarball="$cache/libdrm-$DRM_VER.tar.xz"
src="$cache/libdrm-$DRM_VER"
if [ ! -f "$tarball" ]; then
    echo "[+] fetch libdrm-$DRM_VER"
    curl -fSL --retry 5 --retry-delay 2 --retry-all-errors --connect-timeout 30 \
        -o "$tarball.part" "https://dri.freedesktop.org/libdrm/libdrm-$DRM_VER.tar.xz"
    mv -f "$tarball.part" "$tarball"
fi
[ -d "$src" ] || tar xJf "$tarball" -C "$cache"

echo "=== libdrm $DRM_VER (freedreno-only, arm64 Bionic) ==="
CROSS="$cache/libdrm-android-aarch64.ini"
cat > "$CROSS" <<EOF
[binaries]
c = '$NDKBIN/aarch64-linux-android$API-clang'
cpp = '$NDKBIN/aarch64-linux-android$API-clang++'
ar = '$NDKBIN/llvm-ar'
strip = '$NDKBIN/llvm-strip'
pkg-config = 'pkg-config'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[built-in options]
c_args = ['-D__USE_GNU']
EOF

B="$src/build-android"
rm -rf "$B"
meson setup "$B" "$src" --cross-file "$CROSS" \
  --buildtype=release --default-library=shared --prefix="$SYSROOT" \
  -Dfreedreno=enabled -Dfreedreno-kgsl=true \
  -Damdgpu=disabled -Dradeon=disabled -Dnouveau=disabled -Dvmwgfx=disabled \
  -Dintel=disabled -Dexynos=disabled -Detnaviv=disabled -Dvc4=disabled \
  -Domap=disabled -Dtegra=disabled \
  -Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled \
  -Dudev=false -Dtests=false -Dinstall-test-programs=false >/dev/null

ninja -C "$B" >/dev/null
rm -rf "$SYSROOT"
ninja -C "$B" install >/dev/null

# --- stage runtime .so into the turnip asset dir (deref versioned symlinks) ---
# Only libdrm.so is needed at runtime: the source Turnip ICD + mesa-zink link
# libdrm.so (core DRM ioctls); nothing NEEDs libdrm_freedreno.so (mesa has its
# own src/freedreno/drm). It stays in the sysroot for any build-time linkage.
mkdir -p "$TURNIP_OUT"
for base in libdrm; do
    real="$(readlink -f "$SYSROOT/lib/$base.so")"
    [ -f "$real" ] || { echo "error: $base.so missing after libdrm install" >&2; exit 1; }
    cp -f "$real" "$TURNIP_OUT/$base.so"
    "$NDKBIN/llvm-strip" --strip-unneeded "$TURNIP_OUT/$base.so"
done

# --- verify SONAME + Bionic-cleanliness ------------------------------------
RE="$NDKBIN/llvm-readelf"
echo
for base in libdrm; do
    f="$TURNIP_OUT/$base.so"
    soname="$("$RE" -d "$f" | awk -F'[][]' '/SONAME/{print $2}')"
    needed="$("$RE" -d "$f" | awk -F'[][]' '/NEEDED/{print $2}' | tr '\n' ' ')"
    badver="$("$RE" -V "$f" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+|ZLIB_[0-9.]+' | sort -u | tr '\n' ' ' || true)"
    echo "[=] $base.so ($(du -h "$f" | cut -f1))  SONAME=$soname  NEEDED=$needed"
    [ -z "$badver" ] || { echo "FAIL: non-Bionic verneeds in $base.so: $badver" >&2; exit 1; }
done
echo "[=] OK — source libdrm staged (sysroot: $SYSROOT)"
