#!/usr/bin/env bash
# Fetches Turnip (Mesa's open-source Adreno Vulkan driver) + the Khronos
# Vulkan loader + all runtime deps as Termux arm64 Bionic .deb packages,
# and stages the .so files + the ICD manifest into toolchain/turnip-libs/.
#
# WHY: the system Qualcomm proprietary Adreno driver advertises
# VK_EXT_robustness2 nullDescriptor but REJECTS it at vkCreateDevice, which
# DXVK requires with no fallback → no D3D11 device → AmpliTube (and any
# Direct2D/D3D11 plugin) can't render. winevulkan was audited and faithfully
# forwards the feature, so it's a genuine driver bug. The fix — exactly what
# Winlator does — is to run DXVK on Turnip instead of the proprietary driver.
# Turnip implements robustness2/nullDescriptor correctly. See memory
# feedback_amplitube_blank_render + reference_winlator_gpu_architecture.
#
# All these libs were verified Bionic-clean (llvm-readelf -V shows only
# LIBC/LIBC_N version needs, NOT glibc-style ZLIB_x.x/GLIBC_x.x that Bionic's
# linker can't resolve — unlike Termux's older zlib/freetype, cf.
# fetch-x11-libs.sh). The device (OnePlus 12-class Adreno a7xx) exposes
# /dev/dri/renderD128, so Turnip's well-supported DRM-MSM backend works.
#
# Runtime wiring (done elsewhere): win32u dlopens our bundled Khronos loader
# (vulkan.c SONAME_LIBVULKAN dlopen, redirected via env), with
# VK_ICD_FILENAMES → the staged freedreno_icd.aarch64.json and LD_LIBRARY_PATH
# → the staged libs dir, so winevulkan → loader → Turnip → renderD128.

set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

REPO="https://packages.termux.dev/apt/termux-main"
out="toolchain/turnip-libs"
tmp="$repo_root/.cache/turnip-debs"
mkdir -p "$out" "$tmp"

# Termux package names. Filenames are resolved dynamically from the index so
# version bumps don't break this script.
PKGS=(
  mesa-vulkan-icd-freedreno   # Turnip: libvulkan_freedreno.so + freedreno_icd.aarch64.json
  vulkan-loader-generic       # Khronos Vulkan loader: libvulkan.so.1
  libdrm                      # DRM-MSM kernel access (/dev/dri/renderD128)
  zlib                        # libz.so.1
  zstd                        # libzstd.so.1
  libwayland                  # libwayland-client.so.0 (NEEDED even if WSI unused)
  libxcb                      # libxcb.so + libxcb-{dri3,present,sync,randr,shm,xfixes}.so
  libx11                      # libX11-xcb.so
  libxshmfence                # libxshmfence.so
  libandroid-shmem            # libandroid-shmem.so (Bionic shm shim)
  libffi                      # libffi.so (NEEDED by libwayland-client)
)

idx="$tmp/Packages"
if [ ! -s "$idx" ]; then
  echo "[+] fetch package index"
  curl -fsSL -o "$idx" "$REPO/dists/stable/main/binary-aarch64/Packages"
fi

for pkg in "${PKGS[@]}"; do
  fn=$(awk -v p="^Package: $pkg\$" '$0~p{f=1} f&&/^Filename:/{print $2; exit}' "$idx")
  if [ -z "$fn" ]; then echo "error: package '$pkg' not found in index" >&2; exit 1; fi
  deb="$tmp/$(basename "$fn")"
  if [ ! -f "$deb" ]; then echo "[+] fetch $pkg ($(basename "$fn"))"; curl -fsSL -o "$deb" "$REPO/$fn"; fi
  ar p "$deb" data.tar.xz | tar xJ -C "$tmp" 2>/dev/null
done

usr="$tmp/data/data/com.termux/files/usr"
# Stage every .so (real SONAME names incl .so.N) + the ICD manifest.
cp -av "$usr"/lib/libvulkan_freedreno.so \
       "$usr"/lib/libvulkan.so.1* \
       "$usr"/lib/libdrm.so* \
       "$usr"/lib/libz.so.1* \
       "$usr"/lib/libzstd.so.1* \
       "$usr"/lib/libwayland-client.so* \
       "$usr"/lib/libxcb.so* "$usr"/lib/libxcb-dri3.so* "$usr"/lib/libxcb-present.so* \
       "$usr"/lib/libxcb-sync.so* "$usr"/lib/libxcb-randr.so* "$usr"/lib/libxcb-shm.so* \
       "$usr"/lib/libxcb-xfixes.so* "$usr"/lib/libX11-xcb.so* \
       "$usr"/lib/libxshmfence.so* "$usr"/lib/libandroid-shmem.so* "$usr"/lib/libffi.so* \
       "$out"/ 2>/dev/null || true
cp -av "$usr"/share/vulkan/icd.d/freedreno_icd.aarch64.json "$out"/ 2>/dev/null || true

# Dereference the versioned-SONAME symlinks (libz.so.1 -> libz.so.1.3.2 etc.)
# into REAL files at their SONAME names, and drop the now-redundant versioned
# copies. The runtime extractor (WineSetup, NLS-style TarReader) SKIPS symlinks,
# and Turnip's NEEDED entries + win32u's loader dlopen reference the SONAMEs
# (libz.so.1, libzstd.so.1, libvulkan.so.1), so they must be real files.
( cd "$out"
  for sl in libz.so.1 libzstd.so.1 libvulkan.so.1; do
    if [ -L "$sl" ]; then tgt=$(readlink "$sl"); rm "$sl"; cp "$tgt" "$sl"; fi
  done
  rm -f libz.so.1.* libzstd.so.1.* libvulkan.so.1.* 2>/dev/null || true
)
# Make the ICD manifest's library_path a bare filename so the Khronos loader
# resolves it via LD_LIBRARY_PATH (set to the extracted turnip dir at runtime)
# rather than the Termux absolute path baked in by the .deb.
if command -v python3 >/dev/null; then
  python3 - "$out/freedreno_icd.aarch64.json" <<'PY'
import json,sys
p=sys.argv[1]; d=json.load(open(p)); d["ICD"]["library_path"]="libvulkan_freedreno.so"
json.dump(d,open(p,"w"),indent=4)
PY
fi

echo
echo "[+] staged $(ls "$out" | wc -l) Turnip files in $out/"
echo "    Turnip:   $(ls -la "$out"/libvulkan_freedreno.so 2>/dev/null | awk '{print $5}') bytes"
echo "    manifest: $(cat "$out"/freedreno_icd.aarch64.json 2>/dev/null | tr -d '\n' | head -c 120)"
echo
echo "NOTE: libc++_shared.so comes from the NDK (already shipped). All others"
echo "      above are Bionic-clean Termux builds. Next: bundle into an asset"
echo "      tarball, extract at runtime, redirect win32u's libvulkan dlopen."
