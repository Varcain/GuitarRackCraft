# Wine patches for the FEX pivot

Each `NNNN-name.patch` here is a patch we wrote against upstream wine 11.9
(github.com/wine-mirror/wine, tag `wine-11.9`) to make it cross-compile
and run on Android Bionic arm64. Apply via `scripts/apply-fex-patches.sh`.

Numbering is sequential; the commit that introduces each patch links to
the corresponding section in `docs/fex-pivot-bionic-patches.md` for the
issue context.

We may *read* third-party diffs (Winlator-bionic, MiceWine, Hangover,
bylaws/wine) for reference when stuck, but we do not vendor their patches
verbatim. Every line here is one we wrote and reviewed.
