#!/usr/bin/env bash
# Cross-compiles DXVK for Windows x64 using llvm-mingw. Produces the five
# DXVK DLLs (d3d9, d3d10core, d3d11, d3d8, dxgi) that wine loads as DLL
# overrides to translate D3D calls into Vulkan.
#
# WineSetup.kt copies these into each wineprefix's system32 at first run
# (see seedDxvk()). Without them, plugins doing D3D9/D3D11 calls (most
# JUCE-based ones — AmpCraft, X50II, etc.) crash on CreateDevice.
#
# Output: src/main/assets/dxvk/x64/{d3d8,d3d9,d3d10core,d3d11,dxgi}.dll

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

DXVK_DIR="$repo_root/external/dxvk"
VK_HEADERS="$repo_root/external/Vulkan-Headers/include"

if [ ! -f "$DXVK_DIR/meson.build" ]; then
    echo "error: DXVK submodule not initialized at $DXVK_DIR" >&2
    echo "  run: git submodule update --init --recursive vsthost_lib/external/dxvk" >&2
    exit 1
fi

if [ ! -d "$VK_HEADERS/vulkan" ]; then
    echo "error: Vulkan-Headers submodule not initialized at $VK_HEADERS" >&2
    echo "  run: git submodule update --init --recursive vsthost_lib/external/Vulkan-Headers" >&2
    exit 1
fi

# llvm-mingw provides the x86_64-w64-mingw32 triple. Same toolchain as wine
# PE + FEX-PE builds.
LLVM_MINGW_BIN="$repo_root/external/llvm-mingw/install/bin"
if [ ! -x "$LLVM_MINGW_BIN/x86_64-w64-mingw32-clang" ]; then
    echo "error: llvm-mingw not built at $LLVM_MINGW_BIN" >&2
    echo "  run: bash scripts/setup-fex-pivot.sh first" >&2
    exit 1
fi
export PATH="$LLVM_MINGW_BIN:$PATH"

if ! command -v meson >/dev/null 2>&1; then
    echo "error: meson not installed (pip3 install --user meson)" >&2
    exit 1
fi
if ! command -v ninja >/dev/null 2>&1; then
    echo "error: ninja not installed (apt install ninja-build)" >&2
    exit 1
fi

# --- glslang ---------------------------------------------------------------
# DXVK's meson.build invokes `find_program('glslang', 'glslangValidator')`.
# Recent glslang (14.x+) renamed the executable to plain `glslang`. Build
# from our submoduled source (KhronosGroup/glslang @ 14.3.0). CMake build,
# ~30s. Output: glslang/build/StandAlone/glslang.
GLSLANG_DIR="$repo_root/external/glslang"
GLSLANG_BUILD="$GLSLANG_DIR/build"
GLSLANG_BIN="$GLSLANG_BUILD/StandAlone/glslang"
if [ ! -x "$GLSLANG_BIN" ]; then
    if [ ! -d "$GLSLANG_DIR/External" ]; then
        echo "error: glslang submodule not initialized at $GLSLANG_DIR" >&2
        echo "  run: git submodule update --init --recursive vsthost_lib/external/glslang" >&2
        exit 1
    fi
    echo "=== building glslang (one-shot) ==="
    # update_glslang_sources.py fetches SPIRV-Tools etc. — DXVK doesn't
    # need optimizer, but the binary builds fine either way. Suppress
    # stderr because the script chatters about already-cloned remotes.
    (cd "$GLSLANG_DIR" && python3 update_glslang_sources.py 2>/dev/null || true)
    cmake -S "$GLSLANG_DIR" -B "$GLSLANG_BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$GLSLANG_BUILD" -j"$(nproc)" >/dev/null
fi
export PATH="$(dirname "$GLSLANG_BIN"):$PATH"
echo "[=] glslang: $("$GLSLANG_BIN" --version 2>&1 | head -1)"

# Cross-file in a known location so we can re-use it across runs.
CROSS_FILE="$repo_root/.cache/dxvk-cross-mingw64.ini"
mkdir -p "$(dirname "$CROSS_FILE")"
cat > "$CROSS_FILE" <<'EOF'
[binaries]
c = 'x86_64-w64-mingw32-clang'
cpp = 'x86_64-w64-mingw32-clang++'
ar = 'x86_64-w64-mingw32-ar'
strip = 'x86_64-w64-mingw32-strip'
windres = 'x86_64-w64-mingw32-windres'
pkgconfig = 'x86_64-w64-mingw32-pkgconf'

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

BUILD_DIR="$DXVK_DIR/build-win64"
INSTALL_DIR="$DXVK_DIR/install-win64"

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    echo "=== meson setup (x64) ==="
    # DXVK auto-detects Vulkan-Headers via meson's wrap system; point it at
    # our submoduled Vulkan-Headers via CPATH so the build doesn't try to
    # fetch from upstream.
    CPATH="$VK_HEADERS" \
        meson setup "$BUILD_DIR" "$DXVK_DIR" \
        --cross-file "$CROSS_FILE" \
        --buildtype=release \
        --prefix="$INSTALL_DIR"
fi

echo "=== ninja install ==="
ninja -C "$BUILD_DIR" install

# DXVK installs to <prefix>/bin/{d3d8,d3d9,d3d10core,d3d11,dxgi}.dll
out_assets="$repo_root/src/main/assets/dxvk/x64"
mkdir -p "$out_assets"
copied=0
for dll in d3d8 d3d9 d3d10core d3d11 dxgi; do
    src="$INSTALL_DIR/bin/${dll}.dll"
    if [ -f "$src" ]; then
        cp -f "$src" "$out_assets/"
        copied=$((copied + 1))
    else
        echo "WARN: $src missing after build"
    fi
done
echo "=== staged $copied DXVK DLLs → $out_assets ==="
ls -la "$out_assets/"
