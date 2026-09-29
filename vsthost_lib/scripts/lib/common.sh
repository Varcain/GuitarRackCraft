# vsthost_lib/scripts/lib/common.sh - shared setup for the VST toolchain
# scripts. Source it by absolute path, e.g. after
#   repo_root="$(cd "$(dirname "$0")/.." && pwd)"     # vsthost_lib/
#   . "$repo_root/scripts/lib/common.sh"
#
# Provides:
#   GRC_ROOT             repository root
#   grc_toolchain_prop   read a key from config/toolchain.properties
#   NDK                  $ANDROID_NDK if set, else ~/Android/Sdk/ndk/<ndk.version.vst>

GRC_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

# grc_toolchain_prop <key>: the value of <key> in config/toolchain.properties.
grc_toolchain_prop() {
    awk -F= -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }' \
        "$GRC_ROOT/config/toolchain.properties"
}

NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/$(grc_toolchain_prop ndk.version.vst)}"
