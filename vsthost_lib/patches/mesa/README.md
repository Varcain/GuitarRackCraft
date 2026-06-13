# Mesa-Zink (desktop GL over Vulkan/Turnip) — build recipe

Regenerates the fixed Mesa libs that ship in `src/main/assets/mesa-zink-libs.tar.gz`
(extracted at runtime to `files/wine/mesa/` by `WineSetup.extractMesaZinkLibs`,
gated on `SETUP_VERSION`). These give wine's win32u a real **desktop GL 4.6**
context via zink → Turnip, needed by JUCE desktop-GLSL plugin editors
(AmpliTube, LeCto, WagnerSharp, …). Without them, plugins that init GL crash
`vst_host` with `libEGL fatal: did not find extension DRI_SWRast version 5`.

The built `.so` files live in `toolchain/mesa-zink-libs/` and the packaged
`mesa-zink-libs.tar.gz` — **both gitignored** (large build artifacts). This dir
is the source-of-truth to *regenerate* them. See `project_zink_layer3_state`
memory for the full root-cause history of each fix.

## ► Just run the script
`scripts/build-mesa-zink.sh` does everything below and is wired into
`build-all.sh` (step 8b) — it's the tested, machine-portable path (generates
the meson cross-file from `$ANDROID_NDK` + repo paths, builds, strips, tars).
The manual recipe below is kept as reference/history; prefer the script.
(Corrections the script bakes in vs. an early hand-run: `libexpat.so` is built
by mesa's expat subproject — not Termux; the link needs `-llog -lsync`; the
big libs must be `llvm-strip --strip-unneeded` while `libEGL_vstpoc` is left
as built.)

## Inputs (all tracked here)
- `0001-zink-android-desktop-gl-via-turnip.patch` — the 4 Mesa source fixes
  (dri_target kopper/swrast, eglcurrent EGL_OPENGL_API, zink_screen HW-pdev
  guard, detect_os escape hatch). Applies on the pinned `3rd_party/mesa` HEAD.
- `build-files/android-aarch64.ini` — meson cross-file (NDK r26 / API 28, Bionic
  arm64). ⚠️ absolute NDK + repo paths are hardcoded for this machine
  (`/home/varcain/...`) — adjust if building elsewhere.
- `build-files/android-deps-include/` — hand-authored stubs for Android-private
  headers the NDK lacks (`cutils/*.h`, `log/log.h`).
- `build-files/android-deps-data/` — Termux `libdrm`(+dev) headers/.so/.pc that
  the cross-file's pkg-config resolves at build time (extracted from Termux
  `libdrm`/`libdrm-dev` .debs; captured so the build is self-contained).
- The Turnip Vulkan shim source: `vsthost_lib/src/main/cpp/mesashim/vulkan_turnip_shim.c`.

## Build steps
```sh
M=3rd_party/mesa
# 1. clean submodule + apply the source patch
git -C $M reset --hard HEAD
git -C $M apply $(pwd)/vsthost_lib/patches/mesa/0001-zink-android-desktop-gl-via-turnip.patch
# 2. drop the build files into the submodule working tree
cp vsthost_lib/patches/mesa/build-files/android-aarch64.ini $M/
mkdir -p $M/.android-deps/include $M/.android-deps/data/data/com.termux/files/usr
cp -r vsthost_lib/patches/mesa/build-files/android-deps-include/* $M/.android-deps/include/
cp -r vsthost_lib/patches/mesa/build-files/android-deps-data/*    $M/.android-deps/data/data/com.termux/files/usr/
# 3. meson + ninja (zink-only, no glvnd, no llvm, standalone libEGL)
meson setup $M/build-android-zink $M --cross-file $M/android-aarch64.ini \
  -Dgallium-drivers=zink -Dvulkan-drivers= -Dglvnd=false -Dllvm=disabled \
  -Dplatforms= -Degl=enabled -Dgbm=enabled -Dglx=disabled -Dgles2=enabled \
  -Dgles1=disabled -Dshared-glapi=enabled -Ddri3=disabled
ninja -C $M/build-android-zink \
  src/gallium/targets/dri/libgallium-24.2.8.so src/egl/libEGL.so \
  src/gbm/libgbm.so src/mapi/shared-glapi/libglapi.so
# 4. Turnip Vulkan shim (zink dlsyms vkGet{Instance,Device}ProcAddr from VK_LIBNAME)
NDK=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
$NDK/aarch64-linux-android28-clang -shared -fPIC -fvisibility=hidden \
  -Wl,-soname,libvulkan_vstpoc.so \
  vsthost_lib/src/main/cpp/mesashim/vulkan_turnip_shim.c -o /tmp/libvulkan_vstpoc.so
```

## Stage → asset
```sh
OUT=vsthost_lib/toolchain/mesa-zink-libs
B=$M/build-android-zink
# copy built libs (libEGL gets renamed to avoid shadowing the system libEGL)
cp $B/src/gallium/targets/dri/libgallium-24.2.8.so $OUT/
cp $B/src/gallium/targets/dri/libgallium-24.2.8.so $OUT/dri/zink_dri.so   # zink_dri = libgallium copy
cp $B/src/egl/libEGL.so $OUT/libEGL_vstpoc.so
cp $B/src/gbm/libgbm.so $B/src/mapi/shared-glapi/libglapi.so $OUT/
cp /tmp/libvulkan_vstpoc.so $OUT/
# libexpat.so + libdrm.so come from Termux (fetch-mesa-zink-libs.sh / android-deps-data)
patchelf --set-soname libEGL_vstpoc.so $OUT/libEGL_vstpoc.so
# CRITICAL (fix 6): neutralise DT_VERSYM/VERNEED or Bionic rejects libEGL
python3 -c "import sys; sys.path.insert(0,'vsthost_lib/scripts'); \
  import importlib.util as u; s=u.spec_from_file_location('p','vsthost_lib/scripts/pack-wine-fex.py'); \
  m=u.module_from_spec(s); s.loader.exec_module(m); \
  from pathlib import Path; m.strip_symbol_versions(Path('$OUT/libEGL_vstpoc.so'))"
# tar (bare names, dri/ subdir) → asset, then bump WineSetup SETUP_VERSION
( cd $OUT && tar czf - libgbm.so libexpat.so libdrm.so libvulkan_vstpoc.so \
    libglapi.so libgallium-24.2.8.so libEGL_vstpoc.so dri/zink_dri.so ) \
  > vsthost_lib/src/main/assets/mesa-zink-libs.tar.gz
# bump SETUP_VERSION in WineSetup.kt so a clean install re-extracts
```

## Runtime wiring (already in tree)
- `WineHostProcess::vstpocSetMesaZinkEnv` — `VSTPOC_EGL_LIBRARY=…/mesa/libEGL_vstpoc.so`,
  `LIBGL_ALWAYS_SOFTWARE=1`, `VSTPOC_ZINK_FORCE_HW=1`, `MESA_LOADER_DRIVER_OVERRIDE=zink`,
  `LIBGL_DRIVERS_PATH=…/mesa/dri`, Turnip via libadrenotools (KGSL).
- wine win32u desktop-GL path: `patches/wine/0033-win32u-opengl-android-gles.patch`.
