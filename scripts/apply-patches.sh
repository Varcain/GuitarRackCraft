#!/usr/bin/env bash
# Apply the patches in 3rd_party/patches to their submodules - strictly.
#
#   scripts/apply-patches.sh                     every 3rd_party/patches/<submodule>/*.patch
#   scripts/apply-patches.sh <patch> <src-dir>   one patch to one source tree
#                                                (for build steps that re-apply one)
#
# A patch is either
#   applied    - it reverse-applies, i.e. it is already in the tree: skipped;
#   applicable - it applies: applied now;
#   or neither - the tree is not the one it was made for (upstream moved, a
#                partial or duplicate application, a hand edit): an error.
# The run stops at the first error and shows patch(1)'s dry-run output.
# Exact context is tried first; a patch that only fits with patch(1)'s
# default fuzz is accepted but reported as "(fuzzy)" - its context has
# drifted from the tree and it should be refreshed.
# Patches are plain `patch -p1` diffs (git or unified headers), applied in
# name order.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# apply_one <patch-file> <source-dir>
apply_one() {
    local p="$1" dir="$2" name out fuzz
    name="${p#"$root"/}"
    for fuzz in 0 2; do
        if patch -p1 -R -F"$fuzz" -f -s --dry-run -d "$dir" < "$p" > /dev/null 2>&1; then
            echo "  applied  $name$([ "$fuzz" = 0 ] || echo " (fuzzy)")"
            return 0
        fi
        if patch -p1 -N -F"$fuzz" -f -s --dry-run -d "$dir" < "$p" > /dev/null 2>&1; then
            patch -p1 -N -F"$fuzz" -f -s --no-backup-if-mismatch -d "$dir" < "$p"
            echo "  patched  $name$([ "$fuzz" = 0 ] || echo " (fuzzy)")"
            return 0
        fi
    done
    out="$(patch -p1 -N -f --dry-run -d "$dir" < "$p" 2>&1 || true)"
    echo "error: $name neither applies to nor is applied in ${dir#"$root"/}:" >&2
    sed 's/^/    /' <<< "$out" >&2
    return 1
}

if [ $# -eq 2 ]; then
    patch_file="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
    apply_one "$patch_file" "$2"
    exit 0
fi
if [ $# -ne 0 ]; then
    echo "usage: $0 [<patch> <src-dir>]" >&2
    exit 2
fi

patches_dir="$root/3rd_party/patches"
mapfile -t patches < <(find "$patches_dir" -name '*.patch' | LC_ALL=C sort)
for p in "${patches[@]}"; do
    rel="${p#"$patches_dir"/}"
    apply_one "$p" "$root/3rd_party/${rel%/*}"
done
