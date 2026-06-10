#!/usr/bin/env bash
# Resets the wine-upstream submodule to its pinned commit and applies every
# patch in patches/wine/*.patch in lexicographic order.
#
# Idempotent: reset → apply. Re-running this script is safe and starts from
# a clean tree each time. Untracked files (build dirs, config caches) are
# preserved — reset --hard only reverts tracked sources.
#
# Called from:
#   - scripts/build-wine-android.sh  (Bionic arm64 cross-compile)
#   - scripts/build-wine-pe.sh       (ARM64X PE DLLs)
#
# If a patch fails to apply, this script aborts. That means either:
#   - the wine submodule has been bumped to a commit where the patch no
#     longer fits — adjust the patch to match the new wine tree, OR
#   - the patches list itself is inconsistent (two patches modify the same
#     hunk). Either way: don't proceed with the build.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
WINE_DIR="$repo_root/external/wine-upstream"
PATCH_DIR="$repo_root/patches/wine"

if [ ! -d "$WINE_DIR/.git" ] && [ ! -f "$WINE_DIR/.git" ]; then
    echo "error: wine submodule not initialized at $WINE_DIR" >&2
    echo "  run: git submodule update --init --recursive vsthost_lib/external/wine-upstream" >&2
    exit 1
fi

# Reset to the pinned commit AND clean any untracked files left by a prior
# patch-apply (e.g. new files added by `git apply` like dlls/comdlg32/-
# vstpoc_picker.c) — otherwise a second run hits "file already exists" on
# add-new-file hunks. We preserve the build dirs by name so re-running this
# helper doesn't force a full wine reconfigure on every iteration.
echo "[+] resetting $WINE_DIR to pinned HEAD ($(cd "$WINE_DIR" && git rev-parse --short HEAD))"
git -C "$WINE_DIR" reset --hard HEAD >/dev/null
git -C "$WINE_DIR" clean -fdx \
    -e build-android-arm64 \
    -e build-arm64ec >/dev/null

# Apply every numbered patch in order. Shell glob expands in numeric order
# because the patch names are 001-*, 002-*, ... Find lists them, sort
# normalizes locale just in case.
mapfile -t patches < <(find "$PATCH_DIR" -maxdepth 1 -name '[0-9]*.patch' | LC_ALL=C sort)
if [ "${#patches[@]}" -eq 0 ]; then
    echo "[=] no patches in $PATCH_DIR (clean upstream wine)"
    exit 0
fi

echo "[+] applying ${#patches[@]} patch(es) to wine"
for p in "${patches[@]}"; do
    name="$(basename "$p")"
    if git -C "$WINE_DIR" apply --check "$p" 2>/dev/null; then
        git -C "$WINE_DIR" apply "$p"
        echo "  ✓ $name"
    else
        # Strict, exact-context apply. If a patch is rejected, it is stale
        # (diffed against a different base) — DON'T fuzz it in silently; that
        # hides drift and ships subtly-wrong hunks. Re-export it against the
        # current tree (apply the prior patches, hand-resolve, `git diff`) so a
        # future git apply is exact. See feedback_wine_patch_repack_traps.
        echo "  ! $name — does not apply cleanly (stale base — re-export it)"
        echo "    inspect with: git -C $WINE_DIR apply --3way $p"
        exit 1
    fi
done

echo "[=] all patches applied to wine"
