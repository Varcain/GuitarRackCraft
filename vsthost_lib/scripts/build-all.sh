#!/usr/bin/env bash
# Top-level orchestrator: builds everything vsthost_lib needs to ship,
# from upstream sources, into src/main/{jniLibs,assets}/.
#
# Run this once after `git submodule update --init --recursive`. Each
# underlying script is idempotent — re-running build-all.sh is cheap if
# nothing changed; expensive only on the first run or when source moves.
#
# Pipeline (order matters, each step's output feeds the next):
#   1.  setup-fex-pivot.sh    — verify submodules + build llvm-mingw
#   2.  fetch-x11-libs.sh     — stage Termux X11 libs + headers into toolchain/
#   3.  build-android-libs.sh — libpng + libfreetype for arm64 Bionic
#   4.  build-gnutls-android.sh — libgnutls.so (wine secur32 / TLS)
#   5.  build-wine-pe.sh      — wine PE DLLs (ARM64X); produces wine-tools
#   6.  build-wine-android.sh — wine Unix side for arm64 Bionic
#   7.  build-fex-pe.sh       — FEX-Emu PE DLLs (libarm64ecfex / libwow64fex)
#   8.  build-dxvk.sh         — DXVK D3D-to-Vulkan translation DLLs
#   9.  build-vst-host.sh     — vst_host.exe + vst_host_x86.exe (PE guests)
#   10. build-uihost-stub.sh  — touch-keyboard COM stubs
#   11. pack-wine-fex.py      — stage everything into src/main/{jniLibs,assets}
#
# Steps not invoked here (run manually if needed):
#   - build-vst3-host.sh — requires external/vst3sdk/ (not a submodule yet;
#     fetch from steinbergmedia/vst3sdk if you need VST3 hosting).
#
# Requirements (one-time host setup):
#   - Android NDK r26.1 at $ANDROID_NDK or $HOME/Android/Sdk/ndk/26.1.10909125
#   - apt: build-essential, autoconf, automake, libtool, bison, flex,
#          gettext-base, gperf, pkg-config, gcc-mingw-w64-x86-64,
#          gcc-mingw-w64-i686, g++-mingw-w64-x86-64, cmake, ninja-build,
#          patchelf, python3, python3-pip
#   - python3-pip: meson (for gnutls deps)
#
# Disk requirements: ~10 GB for intermediates, ~1 GB final output.
# Time: first run ~60–90 minutes on a fast laptop (llvm-mingw build dominates).

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

# --- output a clearly-labelled timestamped step header ---------------------
step() {
    local n="$1" name="$2"
    echo
    echo "================================================================="
    printf "[%s] STEP %s — %s\n" "$(date +%H:%M:%S)" "$n" "$name"
    echo "================================================================="
}

# --- run a script, fail with context on error ------------------------------
run_step() {
    local script="$1"
    if ! "$REPO/scripts/$script"; then
        echo
        echo "build-all FAILED at scripts/$script" >&2
        echo "fix the underlying issue, re-run this orchestrator (or just" >&2
        echo "scripts/$script directly) — earlier completed steps are cached" >&2
        exit 1
    fi
}

start=$(date +%s)

step 1  "setup-fex-pivot (submodules + llvm-mingw)"
run_step setup-fex-pivot.sh

step 2  "fetch-x11-libs (Termux X11 client libs)"
run_step fetch-x11-libs.sh

step 3  "build-android-libs (libpng + libfreetype for arm64)"
run_step build-android-libs.sh

step 4  "build-gnutls-android (wine TLS / secur32)"
run_step build-gnutls-android.sh

step 5  "build-wine-pe (ARM64X PE DLLs; also produces wine-tools)"
run_step build-wine-pe.sh

step 6  "build-wine-android (wine Unix side for Bionic arm64)"
run_step build-wine-android.sh

step 7  "build-fex-pe (FEX-Emu PE DLLs)"
run_step build-fex-pe.sh

step 8  "build-dxvk (DXVK D3D-to-Vulkan translation)"
run_step build-dxvk.sh

step 8b "build-mesa-zink (desktop-GL libs for JUCE GL editors → mesa-zink-libs.tar.gz)"
run_step build-mesa-zink.sh

step 9  "build-vst-host (vst_host.exe + vst_host_x86.exe)"
run_step build-vst-host.sh

step 10 "build-uihost-stub (touch-keyboard COM stubs)"
run_step build-uihost-stub.sh

step 11 "pack-wine-fex (stage everything into src/main/jniLibs + assets)"
python3 "$REPO/scripts/pack-wine-fex.py" --repo-root "$REPO"

# Optional: symbol map for crash-address resolution by triage-vst-log.py.
# Reads the build dirs (not cleaned by pack-wine-fex). Never fatal — a
# missing map only means crash addresses stay unresolved in triage output.
step 12 "build-symbol-map (crash-address → function name for triage)"
bash "$REPO/scripts/build-symbol-map.sh" || \
    echo "  (symbol map skipped/failed — non-fatal)"

elapsed=$(( $(date +%s) - start ))
echo
echo "================================================================="
printf "DONE in %dm%02ds. Outputs:\n" $((elapsed/60)) $((elapsed%60))
echo "  src/main/jniLibs/arm64-v8a/  ($(ls "$REPO/src/main/jniLibs/arm64-v8a" 2>/dev/null | wc -l) files)"
echo "  src/main/assets/             ($(ls "$REPO/src/main/assets" 2>/dev/null | wc -l) files)"
echo "================================================================="
