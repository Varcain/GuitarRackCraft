# VST build — prebuilt inventory (migration-to-full-source reference)

Every binary the VST build (`vsthost_lib/scripts/build-all.sh` + `build-vst3-host.sh`)
consumes **without compiling it from source in this build**. Goal: drive each
row to a from-source build. Verified 2026-06-14 against `win_vst_devel`.

**Migration status (2026-06-17): Phase 0 (X11, A1) ✅ · Phase 1 (Khronos loader) ✅
· Phase 2 (Turnip ICD + source libdrm, A2 driver-half) ✅ · A3 (switch mesa-zink
off the checked-in libdrm blob) 🔜 · Phase 3 (AdrenoTools HAL) ⬜.** See "Migration
status & remaining work" at the bottom for the live plan — that section supersedes
the original "suggested order".

Legend: **SHIPPED** = lands in the APK · **HOST** = build-time tool only.

---

## A. SHIPPED prebuilts — primary migration targets

| # | Prebuilt | What / how obtained | Fetched by | Ships as | From-source path |
|---|----------|---------------------|-----------|----------|------------------|
| A1 ✅ | **Termux X11 client libs** (12): libX11, libxcb, libXau, libXdmcp, libXext, libXrender, libXi, libXfixes, libXrandr, libXcursor, libXxf86vm, libandroid-support | prebuilt arm64 Bionic `.deb`s from `packages.termux.dev` | ~~`scripts/fetch-x11-libs.sh`~~ | `jniLibs/libX*.so`, `libxcb.so`, … | **DONE (Phase 0).** wine `--x-libraries` + app jniLibs now link the source `3rd_party/x11` sysroot (libX11 w/ XKB + the 6 extension submodules added, unversioned SONAMEs), built by the dedicated CI `x11` job (`-DX11_ONLY=ON`). `fetch-x11-libs.sh` keeps only headers/fonts. libandroid-support dropped (Termux shim). Commits 899ad53/a2d9d00/87579cd. |
| A2 ◐ | **Turnip** (Mesa Adreno Vulkan driver) **+ Khronos Vulkan loader** (`libvulkan.so.1`) **+ runtime deps** | prebuilt arm64 Bionic `.deb`s from `packages.termux.dev`, staged to `toolchain/turnip-libs/` + the ICD manifest | `scripts/fetch-turnip-libs.sh` (build-all `turnip` phase) | `assets/turnip-libs.tar.gz` (gitignored) | **Loader half DONE (Phase 1):** `libvulkan.so.1` built from `external/Vulkan-Loader` v1.3.296 (`build-vulkan-loader.sh`). **Driver half = Phase 2** (below): build Turnip from the **`3rd_party/mesa`** submodule (`-Dvulkan-drivers=freedreno`) + own its real deps. **HAL = Phase 3.** (Fixes the proprietary-Adreno nullDescriptor bug — see `feedback_amplitube_blank_render`.) |
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

## Migration status & remaining work (updated 2026-06-17)

One `fetch-*.sh` area retired per phase. This section is the live plan.

### ✅ Done
- **Phase 0 — X11 (A1).** Source `3rd_party/x11` sysroot linked by wine + the app;
  dedicated CI `x11` job (`-DX11_ONLY=ON`, builds only the X11/Cairo/Mesa sysroot,
  inits only `3rd_party/{x11,expat,mesa}`). `fetch-x11-libs.sh` keeps headers/fonts
  only. Commits 899ad53 / a2d9d00 / 87579cd.
- **Phase 1 — Khronos Vulkan loader (A2 loader-half).** `libvulkan.so.1` from
  `external/Vulkan-Loader` v1.3.296 via `build-vulkan-loader.sh` (NDK clang +
  `-DCMAKE_SYSTEM_NAME=Linux` → generic `VK_ICD_FILENAMES` discovery, no Android
  HAL, no source patch). Dropped `vulkan-loader-generic` from `fetch-turnip-libs.sh`;
  runs in the `assemble` job's `turnip` phase. SETUP_VERSION 31→32.

### ✅ Phase 2 — Turnip Vulkan ICD + source libdrm (A2 driver-half)
Done. The Turnip ICD is built from the in-tree `3rd_party/mesa` submodule:
- `build-turnip-icd.sh`: `-Dvulkan-drivers=freedreno -Dgallium-drivers=
  -Dplatforms= -Dfreedreno-kmds=msm,kgsl -Dzstd=disabled` + `-static-libstdc++`
  + patch `0002-turnip-icd-bionic-no-display-wsi.patch` (a MESA_FORCE_LINUX
  escape hatch in detect_os.h so DETECT_OS_ANDROID=0 → no AHardwareBuffer/VNDK/
  gralloc code that needs `<vndk/hardware_buffer.h>`, and dropping the VK_KHR_display
  WSI on Android-Bionic hosts since `wsi_common_display.c` uses `pthread_cancel`
  which Bionic lacks). Output `libvulkan_freedreno.so` + `freedreno_icd.aarch64.json`
  (library_path rewritten to the bare filename). This is the DXVK Khronos-loader
  **fallback** path (the Phase-1 loader dlopens it).
- `build-libdrm-android.sh`: source libdrm 2.4.125 (freedreno-only, meson/NDK) →
  `toolchain/drm-android` sysroot + `libdrm.so` into the asset. The ICD's one real
  runtime dep.
- **Result (verified):** `libvulkan_freedreno.so` NEEDs only `libdrm.so` + system
  `libz.so`/`libm`/`libdl`/`libc` — the entire Termux WSI cluster (libxcb*,
  libX11-xcb, libwayland-client, libxshmfence, libandroid-shmem, libffi) +
  libzstd.so.1 + libz.so.1 + libc++_shared are GONE (the `-Dplatforms=` empty
  build never references them). `fetch-turnip-libs.sh` is gutted to ONLY the
  AdrenoTools HAL fetch. `turnip-libs.tar.gz`: 6.6M → 5.2M, now loader + source
  ICD + ICD json + source libdrm + HAL. SETUP_VERSION 32→33.

### 🔜 A3 — switch mesa-zink off the checked-in Termux libdrm blob · effort S · risk M
The source libdrm (`toolchain/drm-android`) now exists, but `build-mesa-zink.sh`
still links + bundles the **checked-in Termux libdrm blob** at
`patches/mesa/build-files/android-deps-data/lib/libdrm*.so`. Remaining work:
point build-mesa-zink's meson cross-file pkg-config at `toolchain/drm-android`,
copy `libdrm.so` from there into `mesa-zink-libs.tar.gz`, delete the blob, and
move `build-libdrm-android` to run before the `mesa` phase (it's currently in
`turnip`, which runs after `mesa`). Deferred from Phase 2 because it touches the
working zink/GL-editor build → wants a full mesa-zink rebuild + on-device GL
editor (AmpliTube) check to verify, separate from the ICD work.

### ⬜ Phase 3 — AdrenoTools HAL Turnip (last fetch + primary GPU path) · effort L · risk M–H
Build the Android-HAL Turnip (`vulkan.ad07xx.so`, exports `HMI`) from the same
mesa submodule: `-Dvulkan-drivers=freedreno -Dplatforms=android
-Dfreedreno-kmds=kgsl,msm -Dandroid-stub=true -Dgallium-drivers=`. Stage under the
soname `WineHostProcess` sets (`VSTPOC_ADRENOTOOLS_DRIVERNAME=vulkan.ad07xx.so`).
- DT_NEEDED audit: the HAL needs only Android **platform** libs (libcutils,
  libhardware, liblog, libnativewindow, libsync, libz, libm/libdl/libc) — all on
  device, **nothing to bundle**.
- Highest device risk: must export `HMI` AND enumerate a pdev under `/dev/kgsl`.
  Keep `Turnip_v26.0.0_R8.zip` as a guarded fallback until the source HAL renders
  AmpliTube's editor on-device; when proven → `fetch-turnip-libs.sh` fully retired.

### Open items / cleanup
- **Vulkan loader CI placement (decision pending):** the loader rebuilds every
  `assemble` run inside `turnip` (~30–60s NDK CMake, uncached). Option: own cached
  job like `x11`/`mesa`. Revisit once Phase 2 lands (the whole turnip phase becomes
  a source build → likely worth its own job then).
- **On-device Phase-1 verify:** confirm a GPU/D3D11 plugin (AmpliTube/BIAS) editor
  still renders with the source loader (LeCto/VST2 doesn't exercise Vulkan).
- **Dead code:** delete `fetch-mesa-zink-libs.sh` (superseded by `build-mesa-zink.sh`).
- **B2 (separate track):** build vst_host/vst3_host/uihost with source
  `external/llvm-mingw` instead of apt mingw-w64.
- **A4 fonts:** low priority — vendor source or fall back to wine's own core fonts.
- Accept B1 (NDK), B3 (Gradle), B4 (host tools) as standard host prerequisites.
