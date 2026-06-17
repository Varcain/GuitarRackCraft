#!/usr/bin/env bash
# Fetch the AdrenoTools HAL-format Turnip driver (vulkan.ad07xx.so) — the ONLY
# remaining prebuilt in the turnip stack, and the PRIMARY GPU path. Staged into
# toolchain/turnip-libs/ alongside the now source-built loader + ICD + libdrm.
#
# Phase 2 of the prebuilt→source migration retired everything else this script
# used to fetch as Termux .debs:
#   - Khronos Vulkan loader  → build-vulkan-loader.sh   (libvulkan.so.1)
#   - Turnip Vulkan ICD      → build-turnip-icd.sh       (libvulkan_freedreno.so + ICD json)
#   - libdrm                 → build-libdrm-android.sh   (libdrm.so)
#   - libz/libzstd/libwayland/libxcb*/libX11-xcb/libxshmfence/libandroid-shmem/libffi
#       → no longer needed: the source ICD is built with no WSI (-Dplatforms=) and
#         no zstd, links libz from the NDK/system, and statically links libc++.
#
# WHY the HAL is still a prebuilt: libvulkan_freedreno.so above is a Khronos ICD
# (vk_icd*), loaded by DXVK via the bundled loader + VK_ICD_FILENAMES — the
# FALLBACK path. The PRIMARY path (WineHostProcess VSTPOC_ADRENOTOOLS_* →
# win32u/vulkan.c + the mesa vkshim) loads Turnip as an ANDROID-HAL driver via
# libadrenotools (a Winlator-style linker-namespace hook giving the driver
# /dev/kgsl access on untrusted_app, where renderD128/DRM is SELinux-denied).
# That needs a HAL build exporting HMI (hw_module_t), NOT an ICD. Mesa-Zink's
# desktop GL (AmpliTube's editor) goes ONLY through this path. Phase 3 will
# source-build the HAL from the mesa submodule (-Dplatforms=android) and retire
# this script. See feedback_amplitube_turnip_driver_name_regression.

set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

out="toolchain/turnip-libs"
tmp="$repo_root/.cache/turnip-debs"
mkdir -p "$out" "$tmp"
CURL_ROBUST=(curl -fsSL --retry 5 --retry-delay 2 --retry-all-errors --connect-timeout 30)

ADRENO_ZIP="Turnip_v26.0.0_R8.zip"
ADRENO_URL="https://github.com/K11MCH1/AdrenoToolsDrivers/releases/download/v26.0.0-rc08/${ADRENO_ZIP}"
adreno_zip="$tmp/$ADRENO_ZIP"
if [ ! -f "$adreno_zip" ]; then
  echo "[+] fetch AdrenoTools HAL Turnip ($ADRENO_ZIP)"
  "${CURL_ROBUST[@]}" -o "$adreno_zip.part" "$ADRENO_URL"
  mv -f "$adreno_zip.part" "$adreno_zip"
fi
# Extract just vulkan.ad07xx.so (python3 zipfile — no unzip dependency). Stage it
# under the soname WineHostProcess sets as VSTPOC_ADRENOTOOLS_DRIVERNAME.
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

echo "[+] AdrenoTools HAL Turnip: $(ls -la "$out"/vulkan.ad07xx.so 2>/dev/null | awk '{print $5}') bytes → $out/vulkan.ad07xx.so"
