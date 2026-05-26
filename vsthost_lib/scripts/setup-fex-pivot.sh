#!/usr/bin/env bash
# Bootstraps the vsthost_lib FEX-pivot build tree.
#
# Sources are managed as git submodules in the parent GuitarRackCraft repo:
#   - wine          external/wine-upstream      (github.com/wine-mirror/wine @ wine-10.10)
#   - FEX-Emu       external/fex-upstream       (github.com/FEX-Emu/FEX     @ 07f7aa3c8)
#   - llvm-mingw    external/llvm-mingw         (github.com/mstorsjo/llvm-mingw @ 20250730)
#
# Android Bionic adaptations live in patches/wine/ and are applied at build
# time by scripts/build-wine-android.sh / scripts/build-wine-pe.sh — NOT by
# this script. setup-fex-pivot.sh only verifies the submodules are present
# and builds the unmodified upstream llvm-mingw toolchain from source.
#
# Re-runnable: skips work that's already complete. Delete a submodule's
# working tree (or remove the install/ marker) to force re-init.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

# Submodule paths within this lib (relative to repo_root which is vsthost_lib).
WINE_DIR="external/wine-upstream"
FEX_DIR="external/fex-upstream"
LLVM_MINGW_DIR="external/llvm-mingw"

require_submodule() {
    local label="$1" path="$2"
    if [ ! -d "$path/.git" ] && [ ! -f "$path/.git" ]; then
        echo "error: $label submodule not initialized at $path" >&2
        echo "  run from the GuitarRackCraft root: git submodule update --init --recursive $path" >&2
        return 1
    fi
    local head
    head=$(cd "$path" && git rev-parse --short HEAD)
    echo "[=] $label: $path @ $head"
}

# --- check submodules are populated -----------------------------------------
require_submodule "wine"        "$WINE_DIR"
require_submodule "fex"         "$FEX_DIR"
require_submodule "llvm-mingw"  "$LLVM_MINGW_DIR"

# --- build llvm-mingw locally (one-shot, ~30-60 min first run) --------------
# Output goes to external/llvm-mingw/install/. Subsequent setup-fex-pivot.sh
# runs detect the install/ tree and skip the build entirely.
LLVM_MINGW_INSTALL="$LLVM_MINGW_DIR/install"
if [ ! -x "$LLVM_MINGW_INSTALL/bin/clang" ]; then
    echo "[+] building llvm-mingw locally — first run takes ~30-60 min and ~6-8 GB intermediates"
    echo "    output: $LLVM_MINGW_INSTALL"
    (
        cd "$LLVM_MINGW_DIR"
        # Upstream build-all.sh takes the install prefix as its sole positional arg.
        # --disable-lldb / --disable-clang-tools-extra trim ~2 GB and ~15 min from
        # the build; we never use either tool. ARM64EC support is in -mingw-w64,
        # not gated by these flags.
        ./build-all.sh \
            --disable-lldb \
            --disable-clang-tools-extra \
            "$(pwd)/install"
    )
else
    echo "[=] llvm-mingw: $LLVM_MINGW_INSTALL/bin/clang already built"
fi

echo
echo "=== ready ==="
echo "  wine:        $WINE_DIR              ($(cd "$WINE_DIR" && cat VERSION))"
echo "  fex:         $FEX_DIR               ($(cd "$FEX_DIR" && git rev-parse --short HEAD))"
echo "  llvm-mingw:  $LLVM_MINGW_INSTALL   ($("$LLVM_MINGW_INSTALL/bin/clang" --version | head -1))"
echo
echo "next steps:"
echo "  scripts/build-wine-android.sh  (applies patches/wine/ then cross-compiles wine for Android)"
echo "  scripts/build-wine-pe.sh       (PE-side ARM64X DLLs)"
echo "  scripts/build-fex-pe.sh        (FEX-Emu PE DLLs)"
echo "  scripts/build-vst-host.sh      (vst_host.exe / vst_host_x86.exe)"
