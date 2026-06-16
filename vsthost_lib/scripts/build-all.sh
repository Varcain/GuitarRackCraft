#!/usr/bin/env bash
# Top-level orchestrator: builds everything vsthost_lib needs to ship,
# from upstream sources, into src/main/{jniLibs,assets}/.
#
# Run this once after `git submodule update --init --recursive`. Each
# underlying script is idempotent — re-running build-all.sh is cheap if
# nothing changed; expensive only on the first run or when source moves.
#
# Usage:
#   build-all.sh                  # run ALL phases in order (full build)
#   build-all.sh <phase>...       # run only the named phase(s), in the
#                                 # order given (used by CI to build each
#                                 # cacheable component as a separate job)
#   build-all.sh -h | --help      # list phases
#
# Phases (default order — each phase's output feeds later ones):
#   llvm      setup-fex-pivot.sh       — verify submodules + build llvm-mingw
#   winedeps  fetch-x11-libs.sh        — stage Termux X11 libs + headers
#             build-android-libs.sh    — libpng + libfreetype for arm64 Bionic
#             build-gnutls-android.sh  — libgnutls.so (wine secur32 / TLS)
#   wine      build-wine-pe.sh         — wine PE DLLs (ARM64X); produces wine-tools
#             build-wine-android.sh    — wine Unix side for arm64 Bionic
#   fex       build-fex-pe.sh          — FEX-Emu PE DLLs (libarm64ecfex / libwow64fex)
#   dxvk      build-dxvk.sh            — DXVK D3D-to-Vulkan translation DLLs
#   mesa      build-mesa-zink.sh       — desktop-GL libs (zink→Turnip) → mesa-zink-libs.tar.gz
#   adrenotools build-adrenotools.sh   — libadrenotools + hook libs (Turnip HAL loader → jniLibs)
#   turnip    fetch-turnip-libs.sh     — Adreno Vulkan driver + Khronos loader → turnip-libs.tar.gz
#   hosts     build-vst-host.sh        — vst_host.exe + vst_host_x86.exe (PE guests)
#             build-vst3-host.sh       — vst3_host.exe (VST3 hosting; needs vst3sdk submodule)
#             build-uihost-stub.sh     — touch-keyboard COM stubs
#   pack      pack-wine-fex.py         — stage everything into src/main/{jniLibs,assets}
#             build-symbol-map.sh      — crash-address → function name (non-fatal)
#
# Toolchain DAG (which phases a phase needs already built):
#   wine, fex, dxvk  need  llvm        wine  also needs  winedeps + NDK
#   mesa             needs NDK only    pack  needs  wine + fex + winedeps (+ dxvk/mesa/turnip/hosts assets)
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

# ===========================================================================
# Phases. Each is an independently-runnable, separately-cacheable unit; the
# step numbers below match the historical single-pipeline numbering.
# ===========================================================================

phase_llvm() {
    step 1 "setup-fex-pivot (submodules + llvm-mingw)"
    run_step setup-fex-pivot.sh
}

phase_winedeps() {
    step 2 "fetch-x11-libs (Termux X11 client libs)"
    run_step fetch-x11-libs.sh

    step 3 "build-android-libs (libpng + libfreetype for arm64)"
    run_step build-android-libs.sh

    step 4 "build-gnutls-android (wine TLS / secur32)"
    run_step build-gnutls-android.sh
}

phase_wine() {
    step 5 "build-wine-pe (ARM64X PE DLLs; also produces wine-tools)"
    run_step build-wine-pe.sh

    step 6 "build-wine-android (wine Unix side for Bionic arm64)"
    run_step build-wine-android.sh
}

phase_fex() {
    step 7 "build-fex-pe (FEX-Emu PE DLLs)"
    run_step build-fex-pe.sh
}

phase_dxvk() {
    step 8 "build-dxvk (DXVK D3D-to-Vulkan translation)"
    run_step build-dxvk.sh
}

phase_mesa() {
    step 8b "build-mesa-zink (desktop-GL libs for JUCE GL editors → mesa-zink-libs.tar.gz)"
    run_step build-mesa-zink.sh
}

phase_adrenotools() {
    step 8d "build-adrenotools (libadrenotools + hook libs — Turnip HAL loader)"
    run_step build-adrenotools.sh
}

phase_turnip() {
    step 8c "fetch-turnip (Adreno Vulkan driver + Khronos loader → turnip-libs.tar.gz)"
    run_step fetch-turnip-libs.sh
    # fetch-turnip-libs only STAGES into toolchain/turnip-libs/ (it's a fetch script,
    # like fetch-x11-libs). Bundle that into the runtime asset here, with bare SONAME
    # filenames (no leading dir) so WineSetup.extractTurnipLibs drops them straight
    # into <wine>/turnip/. Without this asset DXVK falls back to the proprietary
    # Adreno driver → D3D11 plugins (BIAS / AmpliTube) render blank.
    local turnip_out="$REPO/toolchain/turnip-libs"
    local turnip_asset="$REPO/src/main/assets/turnip-libs.tar.gz"
    if [ -d "$turnip_out" ] && [ -n "$(ls -A "$turnip_out" 2>/dev/null)" ]; then
        ( cd "$turnip_out" && tar czf "$turnip_asset" * )
        echo "  → $turnip_asset ($(du -h "$turnip_asset" | cut -f1))"
    else
        echo "build-all FAILED: fetch-turnip-libs produced no files in $turnip_out" >&2
        exit 1
    fi
}

phase_hosts() {
    step 9 "build-vst-host (vst_host.exe + vst_host_x86.exe)"
    run_step build-vst-host.sh

    step 9b "build-vst3-host (vst3_host.exe — VST3 hosting)"
    # vst3_host.exe is REQUIRED for VST3 plugins (BIAS / AmpliTube / TONEX) and is
    # gitignored. external/vst3sdk is a submodule now, so build it as part of the
    # pipeline; skip gracefully if the submodule wasn't checked out (preserves the
    # old "build-all works without vst3sdk" behavior).
    if [ -d "$REPO/external/vst3sdk/public.sdk" ]; then
        run_step build-vst3-host.sh
    else
        echo "  (skipped — external/vst3sdk not checked out; VST3 plugins won't work."
        echo "   run: git submodule update --init vsthost_lib/external/vst3sdk)"
    fi

    step 10 "build-uihost-stub (touch-keyboard COM stubs)"
    run_step build-uihost-stub.sh
}

phase_pack() {
    step 11 "pack-wine-fex (stage everything into src/main/jniLibs + assets)"
    python3 "$REPO/scripts/pack-wine-fex.py" --repo-root "$REPO"

    # Optional: symbol map for crash-address resolution by triage-vst-log.py.
    # Reads the build dirs (not cleaned by pack-wine-fex). Never fatal — a
    # missing map only means crash addresses stay unresolved in triage output.
    step 12 "build-symbol-map (crash-address → function name for triage)"
    bash "$REPO/scripts/build-symbol-map.sh" || \
        echo "  (symbol map skipped/failed — non-fatal)"
}

# ===========================================================================
# Phase dispatch
# ===========================================================================

ALL_PHASES=(llvm winedeps wine fex dxvk mesa adrenotools turnip hosts pack)

usage() {
    echo "usage: build-all.sh [phase...]" >&2
    echo "  no args   run all phases (full build): ${ALL_PHASES[*]}" >&2
    echo "  phase...  run only the named phase(s), in the given order" >&2
    echo "  -h|--help show this message" >&2
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
esac

if [ "$#" -eq 0 ]; then
    phases=("${ALL_PHASES[@]}")
else
    phases=("$@")
    for p in "${phases[@]}"; do
        case " ${ALL_PHASES[*]} " in
            *" $p "*) ;;
            *) echo "error: unknown phase '$p'" >&2; usage; exit 2 ;;
        esac
    done
fi

start=$(date +%s)

for p in "${phases[@]}"; do
    "phase_$p"
done

elapsed=$(( $(date +%s) - start ))
echo
echo "================================================================="
printf "DONE [%s] in %dm%02ds. Outputs:\n" "${phases[*]}" $((elapsed/60)) $((elapsed%60))
echo "  src/main/jniLibs/arm64-v8a/  ($(ls "$REPO/src/main/jniLibs/arm64-v8a" 2>/dev/null | wc -l) files)"
echo "  src/main/assets/             ($(ls "$REPO/src/main/assets" 2>/dev/null | wc -l) files)"
echo "================================================================="
