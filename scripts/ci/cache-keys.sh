#!/usr/bin/env bash
# scripts/ci/cache-keys.sh [<name>...] - the CI cache keys, one "name=key" line
# per component (all of them by default), ready to append to $GITHUB_OUTPUT.
# Run it locally to see what a change does to them.
#
# A key is the component's name, the runner OS and a hash of everything its
# cached output is built from: the pinned submodule commits (from HEAD), the
# scripts, patches and CMake files that build it, the pinned NDK
# (config/toolchain.properties) and what it is built against - so rebuilding
# one also rebuilds what links against it. (llvm, llvm_android, winedeps, fex
# and dxvk keep the formulas they had inline in the workflow, so their caches
# carried over.)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

os="${RUNNER_OS:-Linux}"

# commit <submodule>: the commit HEAD pins it at
commit() { git rev-parse "HEAD:$1" 2>/dev/null || echo none; }
# h16: 16 hex digits of the sha256 of stdin
h16() { sha256sum | cut -c1-16; }
# hash <file-or-dir>...: h16 over the names and contents of the files
hash() {
    find "$@" -type f -print0 | LC_ALL=C sort -z |
        xargs -0 -r sha256sum | h16
}
# prop <key>: a value from config/toolchain.properties
prop() { sed -n "s/^$1=//p" config/toolchain.properties; }

ndk_vst="$(prop ndk.version.vst)"
ndk_native="$(prop ndk.version)"
S=vsthost_lib/scripts

declare -A key
llvm_c="$(commit vsthost_lib/external/llvm-mingw)"
key[llvm]="llvm-$os-$llvm_c"
key[llvm_android]="llvm-android-$os-$(cat $S/fetch-llvm-source.sh $S/build-llvm-android.sh | h16)-ndk$ndk_vst"
key[winedeps]="winedeps-$os-$(cat $S/fetch-x11-libs.sh $S/build-android-libs.sh $S/build-gnutls-android.sh | h16)-ndk$ndk_vst"
# The X11 client libs: the x11 job builds the native target x11_runtime_libs.
key[x11]="x11-$os-$(commit 3rd_party/x11)-$(hash cmake/targets/x11_sysroot.cmake cmake/CMakePresets.json \
    cmake/modules/AndroidNDK.cmake cmake/modules/ExternalBuild.cmake cmake/modules/MesonCrossFile.cmake \
    cmake/modules/WriteIfChanged.cmake)-ndk$ndk_native"
# wine links the X11 libs and the winedeps (freetype, gnutls, ...) and builds
# its PE side with llvm-mingw. Only the top-level numbered patches apply.
key[wine]="wine-$os-$(commit vsthost_lib/external/wine-upstream)-$(
    { find vsthost_lib/patches/wine -maxdepth 1 -name '*.patch' | LC_ALL=C sort | xargs -r cat
      cat $S/build-wine-pe.sh $S/build-wine-android.sh $S/apply-wine-patches.sh
      echo "${key[x11]} ${key[winedeps]} ndk$ndk_vst"; } | h16)-$llvm_c"
key[fex]="fex-$os-$(commit vsthost_lib/external/fex-upstream)-$(cat $S/build-fex-pe.sh | h16)-$llvm_c"
key[dxvk]="dxvk-$os-$(commit vsthost_lib/external/dxvk)-$(commit vsthost_lib/external/Vulkan-Headers)-$(
    commit vsthost_lib/external/glslang)-$(
    { find vsthost_lib/patches/dxvk -type f -name '*.patch' 2>/dev/null | sort | xargs -r cat
      cat $S/build-dxvk.sh; } | h16)-$llvm_c"
# Mesa-Zink: also the Turnip Vulkan shim it compiles, the header stubs, and
# pack-wine-fex.py, whose strip_symbol_versions() it runs on libEGL.
key[mesa]="mesa-$os-$(commit 3rd_party/mesa)-$(hash vsthost_lib/patches/mesa vsthost_lib/src/main/cpp/mesashim \
    $S/build-mesa-zink.sh $S/build-libdrm-android.sh $S/pack-wine-fex.py)-ndk$ndk_vst"
# The native prebuild (build.sh full/playstore build the same targets):
# everything it reads, and every 3rd_party submodule commit HEAD pins (from the
# tree, so it doesn't matter which are initialised).
key[native]="native-$os-$(hash build.sh cmake 3rd_party/patches config scripts/apply-patches.sh \
    plugin_descriptions.json)-$(git ls-tree -r HEAD 3rd_party | awk '$2 == "commit"' | h16)"

names=("$@")
[ ${#names[@]} -gt 0 ] || names=(llvm llvm_android winedeps x11 wine fex dxvk mesa native)
for name in "${names[@]}"; do
    [ -n "${key[$name]:-}" ] || { echo "cache-keys.sh: unknown key '$name'" >&2; exit 2; }
    echo "$name=${key[$name]}"
done
echo "ndk_vst=$ndk_vst"
echo "ndk_native=$ndk_native"
