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

usage() {
    cat <<USAGE
Usage: ./build.sh [<command>]

  full (default), all   everything a local full-flavor build needs: patch, the
                        VST host stack (see BUILD_VST below), native, stage
  playstore             the same without the VST host stack
  native [<target>...]  configure if needed, build all_plugins - or just the
                        given targets, e.g. neuralrack_done - then stage
  stage                 mirror what the native build staged into app/src/ and
                        the asset packs, and write the asset manifests
  patch                 init the native build's submodules, apply
                        3rd_party/patches, generate the FFTW codelets
  vst [<phase>...]      the VST host stack (vsthost_lib/scripts/build-all.sh)
  check                 host tools, the pinned NDK, Meson build dirs
  clean                 remove build outputs and reset the submodules
  help                  this text

The plugins are listed in cmake/plugins.cmake; each has a <name>_done target.
BUILD_VST=0 skips the VST host stack in full/all (the default in CI), and
BUILD_VST=1 forces it. SUBMODULE_DEPTH=<n> initialises submodules shallow.
USAGE
}

# do_check: what the native build needs from the host. Missing tools or the
# wrong NDK fail it; Meson build dirs from another Meson version are only
# listed - they regenerate from scratch on their next change.
do_check() {
    local problems=0 tool ver need

    for tool in cmake ninja meson pkg-config python3 git patch make autoreconf libtoolize; do
        command -v "$tool" > /dev/null || { echo "missing: $tool" >&2; problems=1; }
    done
    for tool in ocaml ocamlbuild; do
        if ! command -v "$tool" > /dev/null; then
            if [ -f "$PROJECT_ROOT/build/fftw3-codelets/generated.tar" ]; then
                echo "note: no $tool (fine while build/fftw3-codelets/generated.tar exists)"
            else
                echo "missing: $tool (generates the FFTW codelets)" >&2
                problems=1
            fi
        fi
    done

    # cmake >= 3.26: ExternalProject INSTALL_BYPRODUCTS (cmake/CMakeLists.txt)
    if command -v cmake > /dev/null; then
        ver="$(cmake --version | awk 'NR == 1 { print $3 }')"
        if [ "$(printf '%s\n' 3.26 "$ver" | sort -V | head -n 1)" != 3.26 ]; then
            echo "cmake $ver is too old: the native build needs 3.26 or newer" >&2
            problems=1
        fi
    fi

    # The pinned NDKs, found the way the builds look for them
    local props="$PROJECT_ROOT/config/toolchain.properties" key ndk rev
    for key in ndk.version ndk.version.vst; do
        need="$(sed -n "s/^$key=//p" "$props")"
        if [ "$key" = ndk.version ]; then
            ndk="${ANDROID_NDK:-${ANDROID_HOME:-$HOME/Android/Sdk}/ndk/$need}"
        else
            ndk="${ANDROID_HOME:-$HOME/Android/Sdk}/ndk/$need"
        fi
        rev=""
        if [ -f "$ndk/source.properties" ]; then
            rev="$(sed -n 's/^Pkg\.Revision *= *//p' "$ndk/source.properties")"
        fi
        if [ "$rev" = "$need" ]; then
            echo "$key $need: $ndk"
        elif [ "$key" = ndk.version ]; then
            echo "$key $need: not at $ndk (found '${rev:-nothing}'); set ANDROID_NDK or ANDROID_HOME" >&2
            problems=1
        else
            echo "note: $key $need (for ./build.sh vst) not at $ndk"
        fi
    done

    # Meson build dirs configured by another Meson version
    if command -v meson > /dev/null && [ -d "$PROJECT_ROOT/build" ]; then
        ver="$(meson --version)"
        local info configured
        while IFS= read -r info; do
            configured="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["meson_version"]["full"])' "$info" 2>/dev/null)" ||
                configured="unknown"
            [ "$configured" = "$ver" ] ||
                echo "note: ${info%/meson-info/meson-info.json} was configured by Meson $configured (now $ver); it regenerates from scratch on its next change"
        done < <(find "$PROJECT_ROOT/build" -maxdepth 4 -path '*/meson-info/meson-info.json')
    fi

    if [ "$problems" -ne 0 ]; then
        echo "check: problems found" >&2
        return 1
    fi
    echo "check: OK"
}

do_clean() {
    echo "Cleaning build directories..."
    # Everything under build/ belongs to the native build (build/prebuild, the
    # ExternalProject build dirs, x11_ui, ...) except build/gradle, the root
    # Gradle project's own output.
    if [ -d "$PROJECT_ROOT/build" ]; then
        find "$PROJECT_ROOT/build" -mindepth 1 -maxdepth 1 ! -name gradle -exec rm -rf {} +
    fi

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
    # What the native build and the staging step write into the source sets:
    # jniLibs, the full overlay, the asset packs, generated assets and the app's
    # LV2 prefix. -X again: only ignored files go, so the tracked plugin
    # screenshots under assets/lv2 stay.
    git -C "$PROJECT_ROOT" clean -fdX \
        app/src/main/jniLibs app/src/full/jniLibs app/src/main/cpp/libs \
        app/src/main/assets app/src/playstore/assets \
        gxplugins_pack/src/main/assets neural_pack/src/main/assets brummer_pack/src/main/assets

    echo "Clean complete."
}

do_patch() {
    # The native build's submodules: everything under 3rd_party but Mesa, which
    # only the VST host stack builds (do_vst). No-op for the ones already
    # initialised.
    git -C "$PROJECT_ROOT" submodule update --init --recursive \
        ${SUBMODULE_DEPTH:+--depth "$SUBMODULE_DEPTH"} -- 3rd_party ':(exclude)3rd_party/mesa'

    # Apply 3rd_party/patches: patches already in a tree are skipped, and one
    # that neither applies nor is applied stops the build (scripts/apply-patches.sh).
    echo "=== Applying 3rd_party patches ==="
    "$PROJECT_ROOT/scripts/apply-patches.sh"

    # ─── Generate FFTW3 codelets (requires OCaml + ocamlbuild) ───────────────
    # The FFTW git repo doesn't ship pre-generated codelet .c files — they require
    # OCaml's genfft. We do an in-source host build with --enable-maintainer-mode,
    # then `make distclean` to remove host objects while keeping the generated codelets.
    # Generated files are cached in build/fftw3-codelets/ so the OCaml build is
    # skipped on subsequent CI runs.
    local fftw_src="$PROJECT_ROOT/3rd_party/fftw3"
    local fftw_codelet_cache="$PROJECT_ROOT/build/fftw3-codelets"

    # Try to restore codelets from cache (submodule checkout wipes generated files)
    if [ ! -f "$fftw_src/dft/scalar/codelets/n1_2.c" ] && [ -f "$fftw_codelet_cache/generated.tar" ]; then
        echo "=== Restoring FFTW3 codelets from cache ==="
        tar xf "$fftw_codelet_cache/generated.tar" -C "$fftw_src"
    fi

    if [ ! -f "$fftw_src/dft/scalar/codelets/n1_2.c" ]; then
        echo "=== Generating FFTW3 codelets (host build with OCaml genfft) ==="
        (
            cd "$fftw_src"
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
        mkdir -p "$fftw_codelet_cache"
        (cd "$fftw_src" && git ls-files --others | tar cf "$fftw_codelet_cache/generated.tar" -T -)
    elif [ ! -f "$fftw_src/configure" ]; then
        # Codelets exist but configure doesn't (e.g. submodule was partially reset)
        (cd "$fftw_src" && touch ChangeLog && autoreconf -fi)
    fi
}

# ─── Windows-VST host stack ──────────────────────────────────────────────────
# The `full` flavor bundles :vsthost_lib (wine + FEX + DXVK + Mesa-Zink + the
# VST hosts). vsthost_lib's orchestrator builds that stack from source and
# stages it into vsthost_lib/src/main/{jniLibs,assets}, so a local `full`/`all`
# build produces a working VST APK. The `playstore` flavor never ships VST.
#
# In CI the heavy components (llvm/wine/fex/dxvk/mesa/...) build as separate
# cached jobs and are staged before build.sh runs, so build-all.sh must NOT run
# there — it would rebuild everything from scratch and defeat the cache. That's
# detected via $CI, and the CI steps also pass BUILD_VST=0 explicitly.
do_vst() {
    # The VST host stack's submodules: vsthost_lib's, and Mesa (Turnip, Zink,
    # lavapipe). No-op for the ones already initialised.
    git -C "$PROJECT_ROOT" submodule update --init --recursive \
        ${SUBMODULE_DEPTH:+--depth "$SUBMODULE_DEPTH"} -- vsthost_lib 3rd_party/mesa
    echo ""
    echo "=== Building Windows-VST host stack (vsthost_lib/scripts/build-all.sh) ==="
    "$PROJECT_ROOT/vsthost_lib/scripts/build-all.sh" "$@"
    echo "=== VST host stack staged into vsthost_lib/src/main/{jniLibs,assets} ==="
}

# do_native [<target>...]: configure if needed, then build the targets
# (default: all_plugins).
do_native() {
    local targets=("$@")
    [ ${#targets[@]} -gt 0 ] || targets=(all_plugins)

    # On exact cache hit, all build outputs (.so files, LV2 assets, jniLibs) are
    # already present from the cache. Skip configure + build entirely.
    # Running ninja would fail anyway because some source files (e.g. back.png)
    # are generated during the build and don't exist in a fresh checkout.
    if [ "${NATIVE_CACHE_EXACT:-}" = "true" ] && [ -f "$BUILD_DIR/build.ninja" ]; then
        echo "=== Exact native cache hit — skipping build, using cached artifacts ==="
        return
    fi

    # Reconfigure if build.ninja is missing or any cmake file changed
    local need_configure=false
    if [ ! -f "$BUILD_DIR/build.ninja" ]; then
        need_configure=true
    elif [ -n "$(find "$CMAKE_DIR" \( -name '*.cmake' -o -name 'CMakeLists.txt' -o -name 'CMakePresets.json' \) -newer "$BUILD_DIR/build.ninja" -print -quit)" ]; then
        need_configure=true
        echo "CMake files changed — reconfiguring..."
        rm -f "$BUILD_DIR/build.ninja"
    elif grep -q '^X11_ONLY:BOOL=ON' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
        # build-all.sh's `x11` phase configures this dir with X11_ONLY=ON (X11
        # sysroot only, no LV2/fftw/plugins). A full build needs those targets
        # back, so force a clean reconfigure with X11_ONLY=OFF.
        need_configure=true
        echo "build dir was configured X11_ONLY=ON — reconfiguring full..."
        rm -f "$BUILD_DIR/build.ninja"
    fi
    if [ "$need_configure" = true ]; then
        echo "=== Configuring (Android arm64-v8a) ==="
        cmake --preset android-arm64 -S "$CMAKE_DIR" -DX11_ONLY=OFF
    fi

    echo "=== Building ${targets[*]} ==="
    cmake --build "$BUILD_DIR" --target "${targets[@]}" -j"$(nproc)"
    echo "=== Build complete ==="
}

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

# write_if_changed <file>: write stdin to <file> unless it already holds
# exactly that, so an unchanged file keeps its mtime.
write_if_changed() {
    cat > "$1.new"
    if cmp -s "$1.new" "$1"; then
        rm -f "$1.new"
    else
        mv -f "$1.new" "$1"
    fi
}

# ─── Mirror the staged libraries into the source sets ───────────────────────
# CMake stages every library the APK ships into $stage/<dir>: core/ for the
# base APK's X11 client libs, and gx/, neural/, brummer/ for the plugins, by
# the Play asset pack cmake/plugins.cmake assigns them. Each destination is made
# to hold exactly its share of the stage - changed files are copied, files no
# longer staged are deleted - on every run, whatever the flavor, so a Gradle
# build of either flavor packages what was just built:
#   app/src/main/jniLibs         core/
#   app/src/full/jniLibs         every plugin lib (full flavor)
#   <pack>/src/main/assets       gx/, neural/, brummer/ (playstore flavor)
#   app/src/playstore/assets/plugin_libs.txt   the packs' file list
do_stage() {
    local stage="$BUILD_DIR/stage" f
    local core gx neural brummer plugins

    echo "=== Staging libraries from $stage ==="
    shopt -s nullglob
    core=("$stage"/core/lib*.so*)
    gx=("$stage"/gx/lib*.so*)
    neural=("$stage"/neural/lib*.so*)
    brummer=("$stage"/brummer/lib*.so*)
    shopt -u nullglob
    plugins=("${gx[@]}" "${neural[@]}" "${brummer[@]}")
    if [ "${#core[@]}" -eq 0 ]; then
        echo "error: no core libraries in $stage/core - nothing has been built" >&2
        exit 1
    fi

    mirror_dir "$PROJECT_ROOT/app/src/main/jniLibs/arm64-v8a" "${core[@]}"
    mirror_dir "$PROJECT_ROOT/app/src/full/jniLibs/arm64-v8a" "${plugins[@]}"
    mirror_dir "$PROJECT_ROOT/gxplugins_pack/src/main/assets/plugins/arm64-v8a" "${gx[@]}"
    mirror_dir "$PROJECT_ROOT/neural_pack/src/main/assets/plugins/arm64-v8a" "${neural[@]}"
    mirror_dir "$PROJECT_ROOT/brummer_pack/src/main/assets/plugins/arm64-v8a" "${brummer[@]}"

    # PluginAssetExtractor reads the packs' file list from this manifest, since
    # assets.list() is unreliable across split APKs.
    mkdir -p "$PROJECT_ROOT/app/src/playstore/assets"
    for f in "${plugins[@]}"; do
        echo "${f##*/}"
    done | write_if_changed "$PROJECT_ROOT/app/src/playstore/assets/plugin_libs.txt"
    rm -f "$PROJECT_ROOT/app/src/main/assets/plugin_libs.txt"  # its old location

    echo "  main: ${#core[@]} core libs; full overlay: ${#plugins[@]} plugin libs"
    echo "  packs: gxplugins ${#gx[@]}, neural ${#neural[@]}, brummer ${#brummer[@]} (plugin_libs.txt)"

    # Generate LV2 asset manifests (all flavors).
    # assets.list() is unreliable across split APKs; extractLV2Assets() reads these instead.
    local assets="$PROJECT_ROOT/app/src/main/assets"
    if [ -d "$assets/lv2" ]; then
        # Bundle directory names (top-level)
        ls -1 "$assets/lv2" | grep '\.lv2$' | write_if_changed "$assets/lv2_bundles.txt"
        echo "  lv2_bundles.txt: $(wc -l < "$assets/lv2_bundles.txt") entries"

        # Comprehensive file manifest (all files under lv2/, relative paths)
        (cd "$assets/lv2" && find . -type f | sed 's|^\./||' | sort) \
            | write_if_changed "$assets/lv2_files.txt"
        echo "  lv2_files.txt: $(wc -l < "$assets/lv2_files.txt") entries"
    fi
    echo "=== Staging complete ==="
}

# no_args "$@": commands that take no arguments reject any
no_args() {
    [ $# -eq 0 ] || { usage >&2; exit 2; }
}

command="${1:-full}"
[ $# -eq 0 ] || shift
case "$command" in
    full|all|playstore)
        no_args "$@"
        do_patch
        vst_default=1
        [ -n "${CI:-}" ] && vst_default=0
        if [ "$command" != playstore ] && [ "${BUILD_VST:-$vst_default}" = 1 ]; then
            do_vst
        fi
        do_native
        do_stage
        ;;
    native)         do_native "$@"; do_stage ;;
    stage)          no_args "$@"; do_stage ;;
    patch)          no_args "$@"; do_patch ;;
    vst)            do_vst "$@" ;;
    check)          no_args "$@"; do_check ;;
    clean)          no_args "$@"; do_clean ;;
    help|-h|--help) usage ;;
    *)              usage >&2; exit 2 ;;
esac
