# VST build — prebuilt inventory (migration-to-full-source reference)

Every binary the VST build (`vsthost_lib/scripts/build-all.sh` + `build-vst3-host.sh`)
consumes **without compiling it from source in this build**. Goal: drive each
row to a from-source build. Verified 2026-06-14 against `win_vst_devel`.

Legend: **SHIPPED** = lands in the APK · **HOST** = build-time tool only.

---

## A. SHIPPED prebuilts — primary migration targets

| # | Prebuilt | What / how obtained | Fetched by | Ships as | From-source path |
|---|----------|---------------------|-----------|----------|------------------|
| A1 | **Termux X11 client libs** (12): libX11, libxcb, libXau, libXdmcp, libXext, libXrender, libXi, libXfixes, libXrandr, libXcursor, libXxf86vm, libandroid-support | prebuilt arm64 Bionic `.deb`s from `packages.termux.dev` | `scripts/fetch-x11-libs.sh` (build-all step 2) | `jniLibs/libX*.so`, `libxcb.so`, … | **Already exists in the main build**: `3rd_party/x11/*` submodules (xorgproto, xtrans, xcb-proto, libXau, libxcb, libX11, libXext, libXrender, pixman, …) are built from source by `./build.sh`. Migration = point wine's `--x-libraries` at the main build's X11 sysroot instead of `toolchain/x11-libs`, and add the missing ones (libXi/libXfixes/libXrandr/libXcursor/libXxf86vm/libandroid-support) as submodules. |
| A2 | **Turnip** (Mesa Adreno Vulkan driver) **+ Khronos Vulkan loader** (`libvulkan.so.1`) **+ runtime deps** | prebuilt arm64 Bionic `.deb`s from `packages.termux.dev`, staged to `toolchain/turnip-libs/` + the ICD manifest | `scripts/fetch-turnip-libs.sh` ⚠️ **run manually — NOT in build-all.sh** | `assets/turnip-libs.tar.gz` (gitignored) | Turnip is a Mesa Vulkan driver → build it from the **`3rd_party/mesa`** submodule already in the tree (`-Dvulkan-drivers=freedreno`), alongside the zink build. Build the loader from `KhronosGroup/Vulkan-Loader` source. (Fixes the proprietary-Adreno nullDescriptor bug — see `feedback_amplitube_blank_render`.) |
| A3 | **libdrm** (`libdrm.so`, `libdrm_freedreno.so`, `libdrm_etnaviv.so`) | **checked-in** Termux blobs at `patches/mesa/build-files/android-deps-data/lib/` | consumed by `scripts/build-mesa-zink.sh` (build-all step 8b) | `libdrm.so` inside `assets/mesa-zink-libs.tar.gz` | Build libdrm from source (its own meson project; or the `libdrm` submodule used by the main build if present). Removes the checked-in blob + its captured headers (`android-deps-data/include/{libdrm,freedreno}`). |
| A4 | **Host fonts** (Liberation + DejaVu `.ttf`) | copied from `/usr/share/fonts/truetype/{liberation,dejavu}` (apt `fonts-liberation`/`fonts-dejavu`) | `scripts/fetch-x11-libs.sh` | `assets/wine-fonts/*.ttf` | Data assets (not "compiled"). Options: vendor the upstream Liberation/DejaVu source tarballs, or rely solely on wine's own core fonts (already built from source during the wine build). Low priority. |

---

## B. HOST toolchain prebuilts — secondary / mostly accepted

| # | Prebuilt | Used for | Note |
|---|----------|----------|------|
| B1 | **Android NDK r26.1.10909125** (Google) | cross-compiling everything arm64 (wine-android, gnutls, png/ft, mesa, pack strip) | Practically un-buildable from source (AOSP toolchain). Accepted host prereq; pin the version. |
| B2 | **APT mingw-w64** (`gcc-mingw-w64-x86-64`, `gcc-mingw-w64-i686`, `g++-mingw-w64-x86-64`, `binutils-mingw-w64-i686`) | `build-vst-host.sh` (→ `vst_host.exe`/`vst_host_x86.exe`) + `build-uihost-stub.sh` + `build-vst3-host.sh` (→ `vst3_host.exe`) | **Replaceable now** — `external/llvm-mingw` is already built from source (`setup-fex-pivot.sh`) and provides x86_64/i686 mingw clang. Migration = build these hosts with llvm-mingw, drop the apt mingw dependency. |
| B3 | **Gradle** (`gradle-<ver>-bin.zip` from services.gradle.org) | the Android app build (wrapper download) | Accepted host tool. |
| B4 | **APT host build tools**: build-essential, autoconf, automake, libtool, bison, flex, gperf, gettext, pkg-config, cmake, ninja-build, meson, patchelf | configure/build of wine, gnutls, mesa, dxvk | Accepted host prereqs (not meaningfully "from source"). |
| B5 | **APT X11 `-dev` headers** (`libx11-dev`, `libxext-dev`, …) | wine `configure` header probes (arch-agnostic; NOT shipped) | Could be sourced from the `3rd_party/x11/*` submodule headers instead (folds into A1). |

---

## C. Already built from source (NOT prebuilts — for completeness)

- **llvm-mingw** — submodule, built by `setup-fex-pivot.sh` (downloads LLVM/mingw-w64/binutils/compiler-rt *source*).
- **wine, FEX-Emu, DXVK, Mesa (zink), glslang, Vulkan-Headers, vst3sdk** — git submodules, compiled.
- **libpng 1.6.43, freetype 2.13.3, gnutls 3.8.6 (+ gmp 6.3.0, nettle 3.10, libtasn1 4.20.0, libunistring 1.2)** — upstream *source* tarballs, compiled (`build-android-libs.sh`, `build-gnutls-android.sh`).
- **vst_host.exe / vst_host_x86.exe / vst3_host.exe / uihost stubs** — our C/C++ source, compiled (currently via apt mingw — see B2).
- **mesa-zink libs** (`libgallium`, `libEGL_vstpoc`, `libgbm`, `libglapi`, `libexpat`, the Turnip shim) — built from the mesa submodule by `build-mesa-zink.sh` (**except** its `libdrm` dep — see A3).
- **wine NLS tables + wine core fonts (tahoma/system)** — built from wine source during the wine build.

---

## Out-of-band assets that were missing from a from-scratch build (FIXED 2026-06-14)

Two gitignored runtime assets were produced by scripts NOT in `build-all.sh`, so
a clean build (incl. the CI workflow) shipped without them:
- **Turnip** (`turnip-libs.tar.gz`) — `fetch-turnip-libs.sh` only staged
  `toolchain/turnip-libs/` and was never wired in → no Turnip → DXVK fell back to
  the proprietary Adreno driver → BIAS/AmpliTube rendered blank. **Fixed:**
  `build-all.sh` step 8c now runs the fetcher + tars the asset.
- **VST3 host** (`vst3_host.exe`) — `build-vst3-host.sh` was excluded (stale
  "vst3sdk not a submodule" reason). **Fixed:** `build-all.sh` step 9b builds it
  (guarded on the now-present `vst3sdk` submodule).

Also: the CI workflow's apt set was missing **`fonts-liberation`/`fonts-dejavu`**,
which `fetch-x11-libs.sh` copies into `wine-fonts/` for wine's Arial/Times/Courier
substitution → **fixed in `build-vst-full.yml`**.

Audit method (re-run when adding any new runtime asset): list every asset
`WineSetup` opens, mark which are gitignored (must be built), and confirm each
gitignored one is produced by a step inside `build-all.sh` / `build.sh full` /
gradle — not a manual/out-of-band script.

---

## Suggested migration order (highest leverage first)

1. **A2 Turnip + loader from the mesa submodule** — removes the biggest prebuilt *and* closes the CI gap; mesa is already in-tree.
2. **A1 X11 libs from `3rd_party/x11` source** — the source path already exists in the main build; mostly wiring.
3. **A3 libdrm from source** — small; removes the last checked-in binary blob and unblocks A2's freedreno backend.
4. **B2 vst hosts via llvm-mingw** — drop the apt mingw toolchain (llvm-mingw already source-built).
5. **A4 fonts** — vendor source or drop to wine's own fonts. Low priority.
6. Accept B1 (NDK), B3 (Gradle), B4 (host tools) as standard host prerequisites.
