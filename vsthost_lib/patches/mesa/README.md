# Mesa-Zink (desktop GL over Vulkan/Turnip) — build recipe

Regenerates the fixed Mesa libs that ship in `src/main/assets/mesa-zink-libs.tar.gz`
(extracted at runtime to `files/wine/mesa/` by `WineSetup.extractMesaZinkLibs`,
gated on `SETUP_VERSION`). These give wine's win32u a real **desktop GL 4.6**
context via zink → Turnip, needed by JUCE desktop-GLSL plugin editors
(AmpliTube, LeCto, WagnerSharp, …). Without them, plugins that init GL crash
`vst_host` with `libEGL fatal: did not find extension DRI_SWRast version 5`.

The built `.so` files live in `toolchain/mesa-zink-libs/` and the packaged
`mesa-zink-libs.tar.gz` — **both gitignored** (large build artifacts). This dir
is the source-of-truth to *regenerate* them.

## Build
`scripts/build-mesa-zink.sh` builds and packages everything; `build-all.sh`
runs it (step 8b). It applies the patches, drops the header stubs into the
submodule, links the source-built libdrm (`toolchain/drm-android`, from
`build-libdrm-android.sh`), generates the meson cross-file from this machine's
NDK and repo paths, builds, strips and tars. (An earlier hand-run recipe with a
committed cross-file and Termux libdrm blobs lived here; see this file's git
history.)

## Inputs (tracked here)
- `0001-zink-android-desktop-gl-via-turnip.patch` — the 4 Mesa source fixes
  (dri_target kopper/swrast, eglcurrent EGL_OPENGL_API, zink_screen HW-pdev
  guard, detect_os escape hatch). Applies on the pinned `3rd_party/mesa` HEAD.
- `0003-zink-accept-cpu-device.patch` — also applied by `build-mesa-zink.sh`.
- `0002-turnip-icd-bionic-no-display-wsi.patch` — for `build-turnip-icd.sh` and
  `build-lavapipe-android.sh`; `0004-llvmpipe-malloc-device-memory-on-android.patch`
  — for `build-lavapipe-android.sh`.
- `build-files/android-deps-include/` — hand-authored stubs for Android-private
  headers the NDK lacks (`cutils/*.h`, `log/log.h`); `build-files/memfd_compat.h`.
- The Turnip Vulkan shim source: `vsthost_lib/src/main/cpp/mesashim/vulkan_turnip_shim.c`.

## Runtime wiring (already in tree)
- `WineHostProcess::vstpocSetMesaZinkEnv` — `VSTPOC_EGL_LIBRARY=…/mesa/libEGL_vstpoc.so`,
  `LIBGL_ALWAYS_SOFTWARE=1`, `VSTPOC_ZINK_FORCE_HW=1`, `MESA_LOADER_DRIVER_OVERRIDE=zink`,
  `LIBGL_DRIVERS_PATH=…/mesa/dri`, Turnip via libadrenotools (KGSL).
- wine win32u desktop-GL path: `patches/wine/0033-win32u-opengl-android-gles.patch`.
