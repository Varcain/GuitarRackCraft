#!/usr/bin/env bash
# Fetches Mesa (Zink gallium driver) + glvnd + X11 deps as Termux arm64 Bionic
# .deb packages and stages them into toolchain/mesa-zink-libs/ for the
# desktop-GL-over-Vulkan (Zink → Turnip) path.
#
# WHY: Android's system EGL is GLES-only. wine 11.9's win32u offscreen-FBO GL
# path runs desktop-GL apps (JUCE editors like AmpliTube) over it, but their
# desktop-GLSL shaders don't compile on a GLES context → blank editor (see
# memory feedback_amplitube_gl_gles_render layer 3). Zink gives a REAL desktop
# GL 4.6 context by translating GL→Vulkan, running on our already-bundled Turnip.
#
# ★The 133MB libLLVM trap: Termux's libgallium bundles llvmpipe and hard-links
# libLLVM.so (133MB, NOT Bionic-clean — needs ZLIB_1.2.0 versioned syms +
# libxml2). Zink NEVER calls LLVM (GL→Vulkan via NIR/SPIR-V). libgallium imports
# only 220 LLVM-C symbols (@LLVM_21.1), all llvmpipe/draw JIT. So we replace the
# real libLLVM with a tiny generated STUB (scripts/build-llvm-stub.sh) exporting
# those 220 symbols versioned + no-op. ~60KB, Bionic-native, no libxml2.
#
# Bionic-cleanliness verified per lib (llvm-readelf -V: only LIBC needs).

set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

REPO="https://packages.termux.dev/apt/termux-main"
out="toolchain/mesa-zink-libs"
tmp="$repo_root/.cache/turnip-debs"   # share the deb cache + Packages index
mkdir -p "$out" "$tmp"

PKGS=(
  mesa        # libEGL_mesa.so, libgallium-NN.so (zink), libgbm.so, dri/libdril_dri.so
  libglvnd    # libEGL.so + libGLdispatch.so (vendor-neutral dispatch; mesa EGL is a glvnd vendor)
  libx11      # libX11.so (libEGL_mesa NEEDED)
  libxext     # libX11 dep
  libxfixes   # mesa dep
  libxxf86vm  # mesa dep
  libxcb-glx  # mesa/glx dep
  libxau           # libxcb dep
  libxdmcp         # libxcb dep
  libandroid-support  # Bionic locale/iconv shim NEEDED by libX11/libxcb
)

idx="$tmp/Packages"
if [ ! -s "$idx" ]; then
  echo "[+] fetch package index"
  curl -fsSL -o "$idx" "$REPO/dists/stable/main/binary-aarch64/Packages"
fi

for pkg in "${PKGS[@]}"; do
  fn=$(awk -v p="^Package: $pkg\$" '$0~p{f=1} f&&/^Filename:/{print $2; exit}' "$idx")
  if [ -z "$fn" ]; then echo "warn: package '$pkg' not found in index, skipping" >&2; continue; fi
  deb="$tmp/$(basename "$fn")"
  if [ ! -f "$deb" ]; then echo "[+] fetch $pkg ($(basename "$fn"))"; curl -fsSL -o "$deb" "$REPO/$fn"; fi
  ar p "$deb" data.tar.xz | tar xJ -C "$tmp" 2>/dev/null
done

usr="$tmp/data/data/com.termux/files/usr"
# Mesa GL/EGL + gallium(zink) + gbm; glvnd dispatch; X11.
cp -av "$usr"/lib/libEGL_mesa.so* \
       "$usr"/lib/libgallium-*.so \
       "$usr"/lib/libgbm.so* \
       "$usr"/lib/libglapi.so* \
       "$usr"/lib/libEGL.so* "$usr"/lib/libGLdispatch.so* "$usr"/lib/libGLESv2.so* \
       "$usr"/lib/libX11.so* "$usr"/lib/libXext.so* "$usr"/lib/libXfixes.so* \
       "$usr"/lib/libXxf86vm.so* "$usr"/lib/libxcb-glx.so* \
       "$usr"/lib/libXau.so* "$usr"/lib/libXdmcp.so* "$usr"/lib/libandroid-support.so* \
       "$out"/ 2>/dev/null || true

# The gallium DRI driver: zink is the megadriver libdril_dri.so. The runtime
# extractor SKIPS symlinks, and MESA_LOADER_DRIVER_OVERRIDE=zink makes mesa
# dlopen "<LIBGL_DRIVERS_PATH>/zink_dri.so", so ship a REAL zink_dri.so.
mkdir -p "$out/dri"
cp -av "$usr"/lib/dri/libdril_dri.so "$out/dri/zink_dri.so" 2>/dev/null || true
cp -av "$usr"/lib/dri/libdril_dri.so "$out/dri/libdril_dri.so" 2>/dev/null || true

# Drop GLX (X11-only, we use surfaceless EGL) to save space if present.
rm -f "$out"/libGLX_mesa.so* 2>/dev/null || true

# Dereference versioned-SONAME symlinks into REAL files at their SONAME (the
# extractor skips symlinks; NEEDED entries reference the SONAMEs).
( cd "$out"
  for f in *.so.[0-9]* dri/*.so.[0-9]*; do
    [ -L "$f" ] || continue
    tgt=$(readlink "$f"); base=$(dirname "$f")
    [ -e "$base/$tgt" ] && { rm "$f"; cp "$base/$tgt" "$f"; }
  done
  # collapse libfoo.so -> libfoo.so.N -> libfoo.so.N.M chains to a real SONAME file
  for sl in libEGL.so.1 libGLdispatch.so.0 libGLESv2.so.2 libglapi.so.0 libgbm.so.1 \
            libX11.so.6 libXext.so.6 libXfixes.so.3 libXxf86vm.so.1 libxcb-glx.so.0; do
    if [ -L "$sl" ]; then tgt=$(readlink "$sl"); rm "$sl"; [ -e "$tgt" ] && cp "$tgt" "$sl"; fi
  done
  rm -f *.so.[0-9]*.[0-9]* 2>/dev/null || true
)

# Copy mesa's shared deps from turnip-libs into the mesa dir so it is
# SELF-CONTAINED. The wine linker searches the mesa dir (it's on LD_LIBRARY_PATH
# and libs dlopen from there), but turnip's dir loads via adrenotools' private
# namespace — NOT LD_LIBRARY_PATH — so mesa can't resolve deps from there.
for d in libandroid-shmem libdrm libz.so.1 libzstd.so.1 libffi libwayland-client \
         libxcb libxcb-dri3 libxcb-present libxcb-sync libxcb-randr libxcb-shm \
         libxcb-xfixes libX11-xcb libxshmfence; do
  cp -a "$repo_root/toolchain/turnip-libs/$d"* "$out"/ 2>/dev/null || true
done

# Build + stage the stub libLLVM.so (replaces the real 133MB one).
"$repo_root/scripts/build-llvm-stub.sh"

echo
echo "[+] staged $(ls "$out" | wc -l) mesa-zink files in $out/:"
ls -la "$out"/ | awk '{print "    "$9"  "$5}' | grep -vE "^    (\.|)$" | head -40
echo "    dri/: $(ls "$out/dri" 2>/dev/null | tr '\n' ' ')"
echo
echo "NOTE: env wiring (runtime): __EGL_VENDOR_LIBRARY_FILENAMES=<dir>/libEGL_mesa.so,"
echo "      MESA_LOADER_DRIVER_OVERRIDE=zink, LIBGL_DRIVERS_PATH=<dir>/dri,"
echo "      GALLIUM_DRIVER=zink, plus Turnip (VK_ICD/adrenotools) + LD_LIBRARY_PATH."
