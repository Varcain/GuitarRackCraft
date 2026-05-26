#!/usr/bin/env bash
# Fetches Termux's prebuilt arm64-android-bionic X11 client libraries and
# X11 protocol headers into toolchain/x11-{libs,headers}/. These are
# build-time inputs for winex11.drv compilation and runtime inputs
# (shipped in jniLibs).
#
# Termux's libraries are built specifically for Android arm64 Bionic —
# they link against libc.so (not glibc's libc.so.6) and use Termux's
# libandroid-support.so shim for POSIX functions that Bionic doesn't
# implement directly. Confirmed to load on OnePlus 12 untrusted_app
# SELinux context.
#
# Versions are pinned to the current stable channel — bump if newer
# upstream is needed. Termux packages mirror upstream X.org closely so
# protocol-level changes between versions are rare.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

# --- headers (arch-agnostic, just protocol definitions) ---------------------
if [ ! -d toolchain/x11-headers/X11 ]; then
  if [ ! -d /usr/include/X11 ]; then
    echo "error: /usr/include/X11 not present. Install libx11-dev:"
    echo "  sudo apt install libx11-dev libxext-dev libxrender-dev libxrandr-dev libxi-dev libxfixes-dev libxcursor-dev libxinerama-dev libxxf86vm-dev"
    exit 1
  fi
  mkdir -p toolchain/x11-headers
  cp -r /usr/include/X11 toolchain/x11-headers/
  echo "[+] copied X11 headers from /usr/include/X11 → toolchain/x11-headers/"
fi

# --- libs (Termux arm64 .deb packages, Bionic-compatible) ---------------------
mkdir -p toolchain/x11-libs
PKG_LIST=(
  "libx11_1.8.13_aarch64.deb         pool/main/libx/libx11/libx11_1.8.13_aarch64.deb"
  "libxau_1.0.12-2_aarch64.deb       pool/main/libx/libxau/libxau_1.0.12-2_aarch64.deb"
  "libxcb_1.17.0-1_aarch64.deb       pool/main/libx/libxcb/libxcb_1.17.0-1_aarch64.deb"
  "libxext_1.3.7_aarch64.deb         pool/main/libx/libxext/libxext_1.3.7_aarch64.deb"
  "libxrender_0.9.12-1_aarch64.deb   pool/main/libx/libxrender/libxrender_0.9.12-1_aarch64.deb"
  "libxi_1.8.3_aarch64.deb           pool/main/libx/libxi/libxi_1.8.3_aarch64.deb"
  "libxfixes_6.0.2_aarch64.deb       pool/main/libx/libxfixes/libxfixes_6.0.2_aarch64.deb"
  "libxrandr_1.5.5_aarch64.deb       pool/main/libx/libxrandr/libxrandr_1.5.5_aarch64.deb"
  "libxcursor_1.2.3-1_aarch64.deb    pool/main/libx/libxcursor/libxcursor_1.2.3-1_aarch64.deb"
  "libxxf86vm_1.1.7_aarch64.deb      pool/main/libx/libxxf86vm/libxxf86vm_1.1.7_aarch64.deb"
  "libxdmcp_1.1.5-2_aarch64.deb      pool/main/libx/libxdmcp/libxdmcp_1.1.5-2_aarch64.deb"
  "libandroid-support_29-1_aarch64.deb pool/main/liba/libandroid-support/libandroid-support_29-1_aarch64.deb"
  # NOTE: freetype, libpng, zlib are built from upstream source against
  # the NDK directly — see scripts/build-android-libs.sh. Termux's
  # versions carry glibc-style symbol versioning (.gnu.version_r with
  # ZLIB_1.2.3.4 etc.) that Bionic's linker can't resolve at runtime
  # ("cannot find 'Export' from verneed[0]", "cannot locate symbol
  # 'inflateInit2_'"). Clean NDK builds avoid the whole mess.
)

tmp="$repo_root/.cache/x11-debs"
mkdir -p "$tmp"

for line in "${PKG_LIST[@]}"; do
  deb=$(echo "$line" | awk '{print $1}')
  url_path=$(echo "$line" | awk '{print $2}')
  if [ ! -f "$tmp/$deb" ]; then
    echo "[+] fetch $deb"
    curl -fsSL -o "$tmp/$deb" "https://packages.termux.dev/apt/termux-main/$url_path"
  fi
done

for deb in "$tmp"/*.deb; do
  ar p "$deb" data.tar.xz | tar xJ -C "$tmp" 2>/dev/null
done

# Copy the .so files we need to toolchain/x11-libs/ with their original
# SONAMEs (Termux uses Bionic-style unversioned names like libX11.so —
# no `.6` suffix).
src="$tmp/data/data/com.termux/files/usr/lib"
cp "$src"/libX11.so "$src"/libXau.so "$src"/libxcb.so \
   "$src"/libXdmcp.so \
   "$src"/libXext.so "$src"/libXrender.so "$src"/libXi.so \
   "$src"/libXfixes.so "$src"/libXrandr.so "$src"/libXcursor.so \
   "$src"/libXxf86vm.so "$src"/libandroid-support.so \
   toolchain/x11-libs/

# Liberation + DejaVu TTFs come from the dev host's font install
# (Termux doesn't package them). Wine needs at least Tahoma /
# LiberationSans / LiberationSerif / LiberationMono to render plugin
# UIs that ask for those by name.
fonts_src=()
for d in /usr/share/fonts/truetype/liberation /usr/share/fonts/truetype/dejavu; do
    if [ -d "$d" ]; then fonts_src+=("$d"); fi
done
if [ ${#fonts_src[@]} -gt 0 ]; then
    mkdir -p toolchain/wine-fonts
    for d in "${fonts_src[@]}"; do
        cp -n "$d"/*.ttf toolchain/wine-fonts/ 2>/dev/null || true
    done
    echo "[+] staged $(ls toolchain/wine-fonts/*.ttf 2>/dev/null | wc -l) host fonts in toolchain/wine-fonts/"
else
    echo "[!] no Liberation/DejaVu host fonts found — wine will render text in fallback fonts only"
fi

echo
echo "NOTE: libfreetype + libpng + libz are built from upstream source"
echo "      against the NDK — run scripts/build-android-libs.sh after this."

echo "[+] staged $(ls toolchain/x11-libs/*.so | wc -l) X11 libs in toolchain/x11-libs/"
echo
echo "next: scripts/build-wine-android.sh   (configure detects X11 from these)"
