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
    rm -rf "$PROJECT_ROOT/build/doubletracker"
    rm -rf "$PROJECT_ROOT/build/lv2_wrapper"

    echo "Restoring 3rd_party to pristine state..."
    # Reset all submodules (undoes patches, waf modifications, generated codelets, etc.)
    git -C "$PROJECT_ROOT" submodule foreach --recursive 'git checkout -- . 2>/dev/null; git clean -fdx 2>/dev/null' || true

    echo "Cleaning vsthost_lib build outputs..."
    # The submodule reset above wipes vsthost_lib/external/* build dirs. This
    # clears the superproject-level VST outputs that build-all.sh stages into
    # gitignored dirs (src/main/{jniLibs,assets}/, toolchain/, .cache/,
    # scripts/__pycache__/). -X removes only ignored files, so tracked source
    # and any local WIP under vsthost_lib/ are preserved.
    git -C "$PROJECT_ROOT" clean -fdX vsthost_lib

    echo "Cleaning staged app outputs..."
    # What the native build and the staging step below write into the source
    # sets: jniLibs, the full overlay, the asset packs, generated assets and the
    # app's LV2 prefix. -X again: only ignored files go, so the tracked plugin
    # screenshots under assets/lv2 stay.
    git -C "$PROJECT_ROOT" clean -fdX \
        app/src/main/jniLibs app/src/full/jniLibs app/src/main/cpp/libs \
        app/src/main/assets app/src/playstore/assets \
        gxplugins_pack/src/main/assets neural_pack/src/main/assets brummer_pack/src/main/assets

    echo "Clean complete."
    exit 0
fi

# Initialize submodules (no-op if already inited)
git -C "$PROJECT_ROOT" submodule update --init --recursive

# Apply 3rd_party/patches: patches already in a tree are skipped, and one
# that neither applies nor is applied stops the build (scripts/apply-patches.sh).
echo "=== Applying 3rd_party patches ==="
"$PROJECT_ROOT/scripts/apply-patches.sh"

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

# ─── Windows-VST host stack (full / all flavors) ─────────────────────────────
# The `full` flavor bundles :vsthost_lib (wine + FEX + DXVK + Mesa-Zink + the
# VST hosts). Build that stack from source via vsthost_lib's orchestrator and
# stage it into vsthost_lib/src/main/{jniLibs,assets} so a local `full`/`all`
# build produces a working VST APK. The `playstore` flavor never ships VST.
#
# In CI the heavy components (llvm/wine/fex/dxvk/mesa/...) build as separate
# cached jobs and are staged before build.sh runs, so build-all.sh must NOT run
# here — it would rebuild everything from scratch and defeat the cache. That's
# detected via $CI, and the CI steps also pass BUILD_VST=0 explicitly. Override:
#   BUILD_VST=0 ./build.sh full   # skip the VST stack (iterate on LV2/native)
#   BUILD_VST=1 ./build.sh full   # force it
_vst_default=1
[ -n "${CI:-}" ] && _vst_default=0
if [ "$FLAVOR" != "playstore" ] && [ "${BUILD_VST:-$_vst_default}" = "1" ]; then
    echo ""
    echo "=== Building Windows-VST host stack (vsthost_lib/scripts/build-all.sh) ==="
    "$PROJECT_ROOT/vsthost_lib/scripts/build-all.sh"
    echo "=== VST host stack staged into vsthost_lib/src/main/{jniLibs,assets} ==="
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
    elif grep -q '^X11_ONLY:BOOL=ON' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
        # build-all.sh's `x11` phase configures this dir with X11_ONLY=ON (X11
        # sysroot only, no LV2/fftw/plugins). A full build needs those targets
        # back, so force a clean reconfigure with X11_ONLY=OFF.
        NEED_CONFIGURE=true
        echo "build dir was configured X11_ONLY=ON — reconfiguring full..."
        rm -f "$BUILD_DIR/build.ninja"
    fi
    if [ "$NEED_CONFIGURE" = true ]; then
        echo "=== Configuring (Android arm64-v8a) ==="
        cmake --preset android-arm64 -S "$CMAKE_DIR" -DX11_ONLY=OFF
    fi

    echo "=== Building all targets ==="
    cmake --build "$BUILD_DIR" --target all_plugins -j"$(nproc)"
    echo "=== Build complete ==="
fi

# ─── Mirror the staged libraries into the source sets ───────────────────────
# CMake stages every library the APK ships into $STAGE/<dir>: core/ for the
# base APK's X11 client libs, and gx/, neural/, brummer/ for the plugins, by
# the Play asset pack cmake/plugins.cmake assigns them. Each destination is made
# to hold exactly its share of the stage - changed files are copied, files no
# longer staged are deleted - on every run, whatever the flavor, so a Gradle
# build of either flavor packages what was just built:
#   app/src/main/jniLibs         core/
#   app/src/full/jniLibs         every plugin lib (full flavor)
#   <pack>/src/main/assets       gx/, neural/, brummer/ (playstore flavor)
#   app/src/playstore/assets/plugin_libs.txt   the packs' file list
STAGE="$BUILD_DIR/stage"
MAIN_JNILIBS="$PROJECT_ROOT/app/src/main/jniLibs/arm64-v8a"
FULL_JNILIBS="$PROJECT_ROOT/app/src/full/jniLibs/arm64-v8a"
GX_PACK="$PROJECT_ROOT/gxplugins_pack/src/main/assets/plugins/arm64-v8a"
NEURAL_PACK="$PROJECT_ROOT/neural_pack/src/main/assets/plugins/arm64-v8a"
BRUMMER_PACK="$PROJECT_ROOT/brummer_pack/src/main/assets/plugins/arm64-v8a"
PLUGIN_LIBS_TXT="$PROJECT_ROOT/app/src/playstore/assets/plugin_libs.txt"

# mirror_dir <dest> [<file>...]: make <dest> hold exactly these files. Only
# files whose content differs are copied, so the rest keep their mtime.
mirror_dir() {
    local dest="$1" f name
    shift
    local -A keep=()
    mkdir -p "$dest"
    for f in "$@"; do
        name="${f##*/}"
        if [ -n "${keep["$name"]:-}" ]; then
            echo "error: two staged libraries are named $name" >&2
            exit 1
        fi
        keep["$name"]=1
        cmp -s "$f" "$dest/$name" || cp -f "$f" "$dest/$name"
    done
    for f in "$dest"/*; do
        [ -e "$f" ] || continue
        [ -n "${keep["${f##*/}"]:-}" ] || rm -f "$f"
    done
}

echo "=== Staging libraries from $STAGE ==="
shopt -s nullglob
core=("$STAGE"/core/lib*.so*)
gx=("$STAGE"/gx/lib*.so*)
neural=("$STAGE"/neural/lib*.so*)
brummer=("$STAGE"/brummer/lib*.so*)
shopt -u nullglob
plugins=("${gx[@]}" "${neural[@]}" "${brummer[@]}")
if [ "${#core[@]}" -eq 0 ]; then
    echo "error: no core libraries in $STAGE/core - nothing has been built" >&2
    exit 1
fi

mirror_dir "$MAIN_JNILIBS" "${core[@]}"
mirror_dir "$FULL_JNILIBS" "${plugins[@]}"
mirror_dir "$GX_PACK" "${gx[@]}"
mirror_dir "$NEURAL_PACK" "${neural[@]}"
mirror_dir "$BRUMMER_PACK" "${brummer[@]}"

# PluginAssetExtractor reads the packs' file list from this manifest, since
# assets.list() is unreliable across split APKs. Rewritten only on change.
mkdir -p "${PLUGIN_LIBS_TXT%/*}"
for f in "${gx[@]}" "${neural[@]}" "${brummer[@]}"; do
    echo "${f##*/}"
done > "$PLUGIN_LIBS_TXT.new"
if cmp -s "$PLUGIN_LIBS_TXT.new" "$PLUGIN_LIBS_TXT"; then
    rm -f "$PLUGIN_LIBS_TXT.new"
else
    mv -f "$PLUGIN_LIBS_TXT.new" "$PLUGIN_LIBS_TXT"
fi
rm -f "$PROJECT_ROOT/app/src/main/assets/plugin_libs.txt"  # its old location

echo "  main: ${#core[@]} core libs; full overlay: ${#plugins[@]} plugin libs"
echo "  packs: gxplugins ${#gx[@]}, neural ${#neural[@]}, brummer ${#brummer[@]} (plugin_libs.txt)"

# Generate LV2 asset manifests (all flavors).
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
echo "=== Staging complete ==="
