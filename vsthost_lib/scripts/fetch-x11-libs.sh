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

# --- X11 client libs: now SOURCE-BUILT, not prebuilt -------------------------
# The X11 client stack (libX11/libxcb/libXau/libXext/libXrender + the
# libXi/libXfixes/libXrandr/libXcursor/libXxf86vm/libXdmcp extensions) is built
# from source by the native cmake X11 sysroot (cmake/targets/x11_sysroot.cmake
# → 3rd_party/x11 submodules) and staged into jniLibs by build.sh — the same
# libs the LV2 GUIs use, with unversioned SONAMEs + XKB enabled. wine's
# winex11.drv links/dlopens them (build-wine-android.sh --x-libraries points at
# build/x11_ui/sysroot). We no longer download Termux .deb X11 prebuilts, and
# the Termux libandroid-support POSIX shim is no longer needed.
# This dir still exists so build-android-libs.sh can stage the source-built
# libfreetype.so / libpng16.so here for wine's gdi32/win32u link.
mkdir -p toolchain/x11-libs

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
