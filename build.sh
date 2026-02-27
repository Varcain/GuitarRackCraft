#!/bin/bash

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# This file is part of Guitar RackCraft.
#
# Guitar RackCraft is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Guitar RackCraft is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_ROOT/build/prebuild"
CMAKE_DIR="$PROJECT_ROOT/cmake"

FLAVOR="${1:-full}"

if [ "$FLAVOR" = "clean" ]; then
    echo "Cleaning build directories..."
    rm -rf "$BUILD_DIR"
    rm -rf "$PROJECT_ROOT/build/aidadsp"
    rm -rf "$PROJECT_ROOT/build/aidax_full"
    rm -rf "$PROJECT_ROOT/build/nam"
    rm -rf "$PROJECT_ROOT/build/lv2"
    rm -rf "$PROJECT_ROOT/build/x11_ui"
    rm -rf "$PROJECT_ROOT/build/mesa"
    rm -rf "$PROJECT_ROOT/build/fftw3"
    rm -rf "$PROJECT_ROOT/build/fftw3-codelets"
    rm -rf "$PROJECT_ROOT/build/neuralrack"
    rm -rf "$PROJECT_ROOT/build/impulseloader"
    rm -rf "$PROJECT_ROOT/build/xdarkterror"
    rm -rf "$PROJECT_ROOT/build/xtinyterror"
    rm -rf "$PROJECT_ROOT/build/collisiondrive"
    rm -rf "$PROJECT_ROOT/build/metaltone"
    rm -rf "$PROJECT_ROOT/build/gxcabsim"
    rm -rf "$PROJECT_ROOT/build/modamptk"
    rm -rf "$PROJECT_ROOT/build/fatfrog"
    rm -rf "$PROJECT_ROOT/build/lv2_wrapper"

    echo "Restoring 3rd_party to pristine state..."
    # Reset all submodules (undoes patches, waf modifications, generated codelets, etc.)
    git -C "$PROJECT_ROOT" submodule foreach --recursive 'git checkout -- . 2>/dev/null; git clean -fdx 2>/dev/null' || true

    echo "Clean complete."
    exit 0
fi

# Initialize submodules (no-op if already inited)
git -C "$PROJECT_ROOT" submodule update --init --recursive

# Apply patches (skip if already applied via dry-run check)
PATCHES_DIR="$PROJECT_ROOT/3rd_party/patches"
if [ -d "$PATCHES_DIR" ]; then
    for p in $(find "$PATCHES_DIR" -name "*.patch" | sort); do
        rel="${p#$PATCHES_DIR/}"
        submod_rel="${rel%/*}"
        submod_dir="$PROJECT_ROOT/3rd_party/$submod_rel"
        if [ -d "$submod_dir" ]; then
            if ! patch -p1 --forward --dry-run -d "$submod_dir" < "$p" >/dev/null 2>&1; then
                continue
            fi
            patch -p1 --forward --no-backup-if-mismatch -d "$submod_dir" < "$p" >/dev/null 2>&1 || true
        fi
    done
fi

# ─── Generate FFTW3 codelets (requires OCaml + ocamlbuild) ───────────────────
# The FFTW git repo doesn't ship pre-generated codelet .c files — they require
# OCaml's genfft. We do an in-source host build with --enable-maintainer-mode,
# then `make distclean` to remove host objects while keeping the generated codelets.
# Generated files are cached in build/fftw3-codelets/ so the OCaml build is
# skipped on subsequent CI runs.
FFTW_SRC="$PROJECT_ROOT/3rd_party/fftw3"
FFTW_CODELET_CACHE="$PROJECT_ROOT/build/fftw3-codelets"

# Try to restore codelets from cache (submodule checkout wipes generated files)
if [ ! -f "$FFTW_SRC/dft/scalar/codelets/n1_2.c" ] && [ -f "$FFTW_CODELET_CACHE/generated.tar" ]; then
    echo "=== Restoring FFTW3 codelets from cache ==="
    tar xf "$FFTW_CODELET_CACHE/generated.tar" -C "$FFTW_SRC"
fi

if [ ! -f "$FFTW_SRC/dft/scalar/codelets/n1_2.c" ]; then
    echo "=== Generating FFTW3 codelets (host build with OCaml genfft) ==="
    (
        cd "$FFTW_SRC"
        [ -f configure ] || { touch ChangeLog; autoreconf -fi; }
        ./configure --enable-maintainer-mode --disable-shared \
            --disable-threads --disable-fortran --disable-mpi --disable-openmp \
            --disable-doc
        make -j"$(nproc)"
        make distclean
    )
    echo "=== FFTW3 codelets generated ==="

    # Cache generated files (codelets + autoreconf outputs) for future runs
    echo "=== Caching FFTW3 generated files ==="
    mkdir -p "$FFTW_CODELET_CACHE"
    (cd "$FFTW_SRC" && git ls-files --others | tar cf "$FFTW_CODELET_CACHE/generated.tar" -T -)
elif [ ! -f "$FFTW_SRC/configure" ]; then
    # Codelets exist but configure doesn't (e.g. submodule was partially reset)
    (cd "$FFTW_SRC" && touch ChangeLog && autoreconf -fi)
fi

# On exact cache hit, all build outputs (.so files, LV2 assets, jniLibs) are
# already present from the cache. Skip configure + build entirely.
# Running ninja would fail anyway because some source files (e.g. back.png)
# are generated during the build and don't exist in a fresh checkout.
if [ "${NATIVE_CACHE_EXACT:-}" = "true" ] && [ -f "$BUILD_DIR/build.ninja" ]; then
    echo "=== Exact native cache hit — skipping build, using cached artifacts ==="
else
    # Reconfigure if build.ninja is missing or any cmake file changed
    NEED_CONFIGURE=false
    if [ ! -f "$BUILD_DIR/build.ninja" ]; then
        NEED_CONFIGURE=true
    elif [ -n "$(find "$CMAKE_DIR" \( -name '*.cmake' -o -name 'CMakeLists.txt' -o -name 'CMakePresets.json' \) -newer "$BUILD_DIR/build.ninja" -print -quit)" ]; then
        NEED_CONFIGURE=true
        echo "CMake files changed — reconfiguring..."
        rm -f "$BUILD_DIR/build.ninja"
    fi
    if [ "$NEED_CONFIGURE" = true ]; then
        echo "=== Configuring (Android arm64-v8a) ==="
        cmake --preset android-arm64 -S "$CMAKE_DIR"
    fi

    echo "=== Building all targets ==="
    cmake --build "$BUILD_DIR" --target all_plugins -j"$(nproc)"
    echo "=== Build complete ==="
fi

# ─── Partition .so files into core vs plugin for Play Store dual-build ────────

# Core libs that stay in main jniLibs (base module)
is_core_lib() {
    local name="$1"
    case "$name" in
        libguitarrackcraft.so) return 0 ;;
        libc++_shared.so)      return 0 ;;
        liblilv-0.so*)         return 0 ;;
        libX11.so*)            return 0 ;;
        libxcb.so*)            return 0 ;;
        libXau.so*)            return 0 ;;
        libGL.so*)             return 0 ;;
        libglapi.so*)          return 0 ;;
        *)                     return 1 ;;
    esac
}

# Classify plugin .so into asset packs
classify_plugin() {
    local name="$1"
    case "$name" in
        libgx_*)                          echo "gx" ;;
        libneural_amp_modeler.so)         echo "neural" ;;
        libAIDA-X*.so)                    echo "neural" ;;
        librt-neural-generic.so)          echo "neural" ;;
        libNeuralrack*.so|libNeuralRack*) echo "neural" ;;
        libCollisionDrive*.so)            echo "brummer" ;;
        libFatFrog*.so)                   echo "brummer" ;;
        libMetalTone*.so)                 echo "brummer" ;;
        libXDarkTerror*.so)               echo "brummer" ;;
        libXTinyTerror*.so)               echo "brummer" ;;
        libImpulseLoader*.so)             echo "brummer" ;;
        libPowerAmp*.so)                  echo "brummer" ;;
        libPreAmp*.so)                    echo "brummer" ;;
        libGxCabSim*.so)                  echo "brummer" ;;
        libgx_cabinet*.so)               echo "brummer" ;;
        libpoweramps*.so)                 echo "brummer" ;;
        *)                                echo "gx" ;;  # Default: other plugins go to gxplugins
    esac
}

MAIN_JNILIBS="$PROJECT_ROOT/app/src/main/jniLibs/arm64-v8a"
FULL_JNILIBS="$PROJECT_ROOT/app/src/full/jniLibs/arm64-v8a"
GX_PACK="$PROJECT_ROOT/gxplugins_pack/src/main/assets/plugins/arm64-v8a"
NEURAL_PACK="$PROJECT_ROOT/neural_pack/src/main/assets/plugins/arm64-v8a"
BRUMMER_PACK="$PROJECT_ROOT/brummer_pack/src/main/assets/plugins/arm64-v8a"

# ─── Rename SONAME-versioned X11/Mesa libs to standard .so extension ─────────
# Android APK packaging only extracts lib*.so from nativeLibDir. Files like
# libxcb.so.1 are NOT extracted. The SONAMEs are already unversioned (libxcb.so),
# so renaming is safe — the linker matches by SONAME, not filename.
rename_count=0
for versioned in "$MAIN_JNILIBS"/libX11.so.[0-9]* \
                 "$MAIN_JNILIBS"/libxcb.so.[0-9]* \
                 "$MAIN_JNILIBS"/libXau.so.[0-9]* \
                 "$MAIN_JNILIBS"/libGL.so.[0-9]* \
                 "$MAIN_JNILIBS"/libglapi.so.[0-9]*; do
    [ -f "$versioned" ] || continue
    base="${versioned%%.so.*}.so"
    if [ ! -f "$base" ]; then
        mv "$versioned" "$base"
        rename_count=$((rename_count + 1))
    else
        rm -f "$versioned"
    fi
done
[ "$rename_count" -gt 0 ] && echo "Renamed $rename_count SONAME-versioned X11/Mesa libs to .so"

if [ "$FLAVOR" = "playstore" ]; then
    echo "=== Partitioning .so files (playstore) ==="
    mkdir -p "$GX_PACK" "$NEURAL_PACK" "$BRUMMER_PACK"
    NEED_FULL=false
    NEED_PACKS=true
elif [ "$FLAVOR" = "all" ]; then
    echo "=== Partitioning .so files (all flavors) ==="
    mkdir -p "$FULL_JNILIBS" "$GX_PACK" "$NEURAL_PACK" "$BRUMMER_PACK"
    NEED_FULL=true
    NEED_PACKS=true
else
    echo "=== Partitioning .so files (full) ==="
    mkdir -p "$FULL_JNILIBS"
    NEED_FULL=true
    NEED_PACKS=false
fi

plugin_count=0
core_count=0

for so_file in "$MAIN_JNILIBS"/lib*.so*; do
    [ -f "$so_file" ] || continue
    name=$(basename "$so_file")

    if is_core_lib "$name"; then
        core_count=$((core_count + 1))
        continue
    fi

    if [ "$NEED_FULL" = true ]; then
        cp -f "$so_file" "$FULL_JNILIBS/$name"
    fi

    if [ "$NEED_PACKS" = true ]; then
        pack=$(classify_plugin "$name")
        case "$pack" in
            gx)      cp -f "$so_file" "$GX_PACK/$name" ;;
            neural)  cp -f "$so_file" "$NEURAL_PACK/$name" ;;
            brummer) cp -f "$so_file" "$BRUMMER_PACK/$name" ;;
        esac
    fi

    # Remove from main jniLibs (only core libs stay there)
    rm -f "$so_file"
    plugin_count=$((plugin_count + 1))
done

# Idempotency: if main was already partitioned (re-run or cache hit), derive
# packs from the full overlay which still has all plugin .so files.
if [ "$plugin_count" -eq 0 ] && [ "$NEED_PACKS" = true ] && [ -d "$FULL_JNILIBS" ]; then
    for so_file in "$FULL_JNILIBS"/lib*.so*; do
        [ -f "$so_file" ] || continue
        name=$(basename "$so_file")
        is_core_lib "$name" && continue
        pack=$(classify_plugin "$name")
        case "$pack" in
            gx)      cp -f "$so_file" "$GX_PACK/$name" ;;
            neural)  cp -f "$so_file" "$NEURAL_PACK/$name" ;;
            brummer) cp -f "$so_file" "$BRUMMER_PACK/$name" ;;
        esac
        plugin_count=$((plugin_count + 1))
    done
    [ "$plugin_count" -gt 0 ] && echo "Packs populated from full overlay (main already partitioned)"
fi

echo "Partitioned: $core_count core libs in main, $plugin_count plugin libs moved"
if [ "$NEED_FULL" = true ]; then
    echo "  full overlay: $(ls "$FULL_JNILIBS" 2>/dev/null | wc -l) files"
fi
if [ "$NEED_PACKS" = true ]; then
    echo "  gxplugins_pack: $(ls "$GX_PACK" 2>/dev/null | wc -l) files"
    echo "  neural_pack: $(ls "$NEURAL_PACK" 2>/dev/null | wc -l) files"
    echo "  brummer_pack: $(ls "$BRUMMER_PACK" 2>/dev/null | wc -l) files"

    # Generate manifest of plugin .so files for PluginAssetExtractor.
    # assets.list() is unreliable across split APKs; the extractor reads this instead.
    MANIFEST="$PROJECT_ROOT/app/src/main/assets/plugin_libs.txt"
    mkdir -p "$(dirname "$MANIFEST")"
    : > "$MANIFEST"
    for dir in "$GX_PACK" "$NEURAL_PACK" "$BRUMMER_PACK"; do
        for f in "$dir"/lib*.so*; do
            [ -f "$f" ] || continue
            basename "$f" >> "$MANIFEST"
        done
    done
    manifest_count=$(wc -l < "$MANIFEST")
    echo "  plugin_libs.txt: $manifest_count entries"

    # Generate LV2 asset manifests.
    # assets.list() is unreliable across split APKs; extractLV2Assets() reads these instead.
    LV2_ASSET_DIR="$PROJECT_ROOT/app/src/main/assets/lv2"
    if [ -d "$LV2_ASSET_DIR" ]; then
        # Bundle directory names (top-level)
        LV2_BUNDLES="$PROJECT_ROOT/app/src/main/assets/lv2_bundles.txt"
        ls -1 "$LV2_ASSET_DIR" | grep '\.lv2$' > "$LV2_BUNDLES"
        lv2_count=$(wc -l < "$LV2_BUNDLES")
        echo "  lv2_bundles.txt: $lv2_count entries"

        # Comprehensive file manifest (all files under lv2/, relative paths)
        LV2_FILES="$PROJECT_ROOT/app/src/main/assets/lv2_files.txt"
        (cd "$LV2_ASSET_DIR" && find . -type f | sed 's|^\./||' | sort) > "$LV2_FILES"
        lv2_files_count=$(wc -l < "$LV2_FILES")
        echo "  lv2_files.txt: $lv2_files_count entries"
    fi
fi
echo "=== Partitioning complete ==="
