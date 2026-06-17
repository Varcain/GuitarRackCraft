#!/usr/bin/env bash
# Fetches Turnip (Mesa's open-source Adreno Vulkan driver) + its runtime deps
# as Termux arm64 Bionic .deb packages, and stages the .so files + the ICD
# manifest into toolchain/turnip-libs/. The Khronos Vulkan loader (libvulkan.so.1)
# is NO LONGER fetched here — it's built from source by build-vulkan-loader.sh
# (Phase 1 of the prebuilt→source migration), staged into the same dir.
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
  # NOTE: the Khronos Vulkan loader (libvulkan.so.1) is NO LONGER fetched here —
  # it's built from source by build-vulkan-loader.sh (run right after this script
  # in build-all.sh phase_turnip). See that script + Phase 1 of the source migration.
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
CURL_ROBUST=(curl -fsSL --retry 5 --retry-delay 2 --retry-all-errors --connect-timeout 30)
if [ ! -s "$idx" ]; then
  echo "[+] fetch package index"
  "${CURL_ROBUST[@]}" -o "$idx.part" "$REPO/dists/stable/main/binary-aarch64/Packages"
  mv -f "$idx.part" "$idx"
fi

for pkg in "${PKGS[@]}"; do
  fn=$(awk -v p="^Package: $pkg\$" '$0~p{f=1} f&&/^Filename:/{print $2; exit}' "$idx")
  if [ -z "$fn" ]; then echo "error: package '$pkg' not found in index" >&2; exit 1; fi
  deb="$tmp/$(basename "$fn")"
  if [ ! -f "$deb" ]; then echo "[+] fetch $pkg ($(basename "$fn"))"; \
     "${CURL_ROBUST[@]}" -o "$deb.part" "$REPO/$fn" && mv -f "$deb.part" "$deb"; fi
  ar p "$deb" data.tar.xz | tar xJ -C "$tmp" 2>/dev/null
done

usr="$tmp/data/data/com.termux/files/usr"
# Stage every .so (real SONAME names incl .so.N) + the ICD manifest.
cp -av "$usr"/lib/libvulkan_freedreno.so \
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
# and Turnip's NEEDED entries reference the SONAMEs (libz.so.1, libzstd.so.1),
# so they must be real files. (libvulkan.so.1 is staged here as a real file by
# build-vulkan-loader.sh, which runs next in phase_turnip.)
( cd "$out"
  for sl in libz.so.1 libzstd.so.1; do
    if [ -L "$sl" ]; then tgt=$(readlink "$sl"); rm "$sl"; cp "$tgt" "$sl"; fi
  done
  rm -f libz.so.1.* libzstd.so.1.* 2>/dev/null || true
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

# --- AdrenoTools HAL-format Turnip (the PRIMARY GPU path) --------------------
# libvulkan_freedreno.so above is a Khronos ICD (exports vk_icd*), loaded by DXVK
# via the bundled Khronos loader + VK_ICD_FILENAMES — that is the FALLBACK path.
# The PRIMARY path (WineHostProcess VSTPOC_ADRENOTOOLS_* → win32u/vulkan.c + the
# mesa vkshim) loads Turnip as an ANDROID-HAL driver through libadrenotools, a
# Winlator-style linker-namespace hook that gives the driver /dev/kgsl access on
# untrusted_app (renderD128/DRM is SELinux-denied to us). That needs a HAL build
# exporting HMI (hw_module_t), NOT an ICD. Mesa-Zink's desktop GL (AmpliTube's
# editor) goes ONLY through this path and has no Khronos-loader fallback, so the
# HAL driver is mandatory. Stage the proven AdrenoTools build under the soname
# WineHostProcess sets as VSTPOC_ADRENOTOOLS_DRIVERNAME (vulkan.ad07xx.so).
# Missing it → adrenotools_open_libvulkan returns NULL → zink "failed to choose
# pdev" → BLACK editor. This file is NOT in the Termux packages, so fetch it
# separately. See feedback_amplitube_turnip_driver_name_regression.
ADRENO_ZIP="Turnip_v26.0.0_R8.zip"
ADRENO_URL="https://github.com/K11MCH1/AdrenoToolsDrivers/releases/download/v26.0.0-rc08/${ADRENO_ZIP}"
adreno_zip="$tmp/$ADRENO_ZIP"
if [ ! -f "$adreno_zip" ]; then
  echo "[+] fetch AdrenoTools HAL Turnip ($ADRENO_ZIP)"
  "${CURL_ROBUST[@]}" -o "$adreno_zip.part" "$ADRENO_URL"
  mv -f "$adreno_zip.part" "$adreno_zip"
fi
# Extract just vulkan.ad07xx.so (python3 zipfile — no unzip dependency).
python3 - "$adreno_zip" "$out" <<'PY'
import sys, zipfile, os
with zipfile.ZipFile(sys.argv[1]) as zf, \
     open(os.path.join(sys.argv[2], "vulkan.ad07xx.so"), "wb") as d:
    d.write(zf.read("vulkan.ad07xx.so"))
PY
# Sanity: a HAL driver exports HMI (hw_module_t). An ICD here would dlopen fine
# but fail device enumeration (the regression's "failed to choose pdev").
if command -v llvm-readelf >/dev/null 2>&1; then
  llvm-readelf --dyn-syms "$out/vulkan.ad07xx.so" 2>/dev/null | grep -q ' HMI$' \
    || echo "WARN: $out/vulkan.ad07xx.so does not export HMI — not a HAL build?" >&2
fi

echo
echo "[+] staged $(ls "$out" | wc -l) Turnip files in $out/"
echo "    Turnip ICD:  $(ls -la "$out"/libvulkan_freedreno.so 2>/dev/null | awk '{print $5}') bytes (DXVK Khronos-loader fallback)"
echo "    Turnip HAL:  $(ls -la "$out"/vulkan.ad07xx.so 2>/dev/null | awk '{print $5}') bytes (adrenotools primary path / zink)"
echo "    manifest: $(cat "$out"/freedreno_icd.aarch64.json 2>/dev/null | tr -d '\n' | head -c 120)"
echo
echo "NOTE: libc++_shared.so comes from the NDK (already shipped). All others"
echo "      above are Bionic-clean Termux builds. Next: bundle into an asset"
echo "      tarball, extract at runtime, redirect win32u's libvulkan dlopen."
