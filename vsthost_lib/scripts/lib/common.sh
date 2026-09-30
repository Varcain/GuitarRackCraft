# vsthost_lib/scripts/lib/common.sh - shared setup for the VST toolchain
# scripts. Source it by absolute path, e.g. after
#   repo_root="$(cd "$(dirname "$0")/.." && pwd)"     # vsthost_lib/
#   . "$repo_root/scripts/lib/common.sh"
#
# Provides:
#   GRC_ROOT             repository root
#   grc_toolchain_prop   read a key from config/toolchain.properties
#   NDK                  $ANDROID_NDK if set, else ~/Android/Sdk/ndk/<ndk.version.vst>
#   fetch_verified       download a tarball and check its pinned sha256

GRC_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

# grc_toolchain_prop <key>: the value of <key> in config/toolchain.properties.
grc_toolchain_prop() {
    awk -F= -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }' \
        "$GRC_ROOT/config/toolchain.properties"
}

NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/$(grc_toolchain_prop ndk.version.vst)}"

# fetch_verified <url> <sha256> <dest>: download <url> to <dest> unless it is
# already there, and check the file against <sha256> either way - a mismatch is
# an error, for a cached copy too (delete it to download again). The download
# lands in <dest>.part and only a verified file is renamed into place, so an
# interrupted or wrong transfer is never taken for a good cached copy later.
# --retry-all-errors also retries partial transfers (curl 18), which plain
# --retry does not.
fetch_verified() {
    local url="$1" sha256="$2" dest="$3" file="$3" actual
    if [ ! -f "$dest" ]; then
        echo "[+] fetch ${dest##*/}"
        mkdir -p "$(dirname "$dest")"
        curl -fSL --retry 5 --retry-delay 2 --retry-all-errors --connect-timeout 30 \
             -o "$dest.part" "$url"
        file="$dest.part"
    fi
    actual="$(sha256sum "$file" | cut -d' ' -f1)"
    if [ "$actual" != "$sha256" ]; then
        echo "error: ${file##*/} has sha256 $actual, expected $sha256 ($url)" >&2
        [ "$file" = "$dest" ] && echo "       delete $dest to download it again" >&2
        return 1
    fi
    [ "$file" = "$dest" ] || mv -f "$file" "$dest"
}
