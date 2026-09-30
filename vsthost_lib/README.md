# vsthost_lib

In-process Windows VST2/VST3 plugin host for GuitarRackCraft. Built on
native wine-arm64ec + FEX-Emu for x86_64 plugin translation. Ships as
an Android library module consumed by `:app`'s `full` flavor (sideload
distribution, targetSdk=28). The `playstore` flavor (targetSdk=35)
doesn't depend on this module: wine's PE relocations need the
pre-Android-10 SELinux execmod permission, which targetSdk 29+ denies.

## Architecture (brief)

- **`src/main/cpp/vst/{VstFactory,WineVstPlugin}.cpp`** — `IPlugin` bridge
  consumed by `:app` via prefab (the only exported header is
  `src/main/cpp/include/vst/VstFactory.h`). Forks one wine subprocess per
  imported VST; audio flows via SysV shm rings.
- **`app/src/main/cpp/x11/`** — the minimal in-process X11 server, shared
  with `:app`'s LV2 UIs and built into this library too. Wine's
  winex11.drv connects to it over a TCP loopback socket; output is
  blitted to a Compose `SurfaceView` via EGL/GLES2.
- **`src/main/cpp/launcher/WineHostProcess.cpp`** — fork+exec wine,
  wired through `vst_host.exe` (PE32+) which loads the user's plugin DLL.
- **`external/`** — git submodules (wine-upstream, fex-upstream, llvm-mingw,
  dxvk, Vulkan-Loader, Vulkan-Headers, glslang, libadrenotools, vst3sdk) and
  in-tree sources for the Windows-side hosts (vst_host, vst_host_vst3,
  uihost_stub, `shared_layout.h`).
- **`patches/wine/*.patch`** — Bionic / FEX-pivot adaptations applied to
  the wine submodule before configure.
- **`scripts/`** — build pipeline (see below).

## Build from sources

Everything is source-built. There are no committed binaries in this
module; `src/main/{jniLibs,assets}/` are gitignored and populated by the
build pipeline.

### One-time host setup

```bash
# 1. The Android NDKs pinned in config/toolchain.properties: ndk.version.vst
#    (26.1.10909125) for these scripts - found at $ANDROID_NDK or
#    $HOME/Android/Sdk/ndk/<version> - and ndk.version (27.2.12479018) for the
#    Gradle build of this module and :app.
sdkmanager "ndk;26.1.10909125" "ndk;27.2.12479018"

# 2. Cross-compilers + autotools chain
sudo apt install -y \
    build-essential autoconf automake libtool bison flex gettext-base \
    gperf pkg-config cmake ninja-build patchelf \
    gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 \
    g++-mingw-w64-x86-64 \
    python3 python3-pip

# 3. Meson (for gnutls dependency builds)
pip3 install --user meson
```

### One-time submodule fetch

From the GuitarRackCraft root:

```bash
git submodule update --init --recursive
```

Among others this pulls:
- **wine** at tag `wine-11.9` (~150 MB)
- **FEX-Emu** at commit `07f7aa3c8` (~50 MB)
- **llvm-mingw** at tag `20250730` (~13 MB)
- `3rd_party/mesa` (Turnip, zink, lavapipe) and `3rd_party/x11` (the X11
  client libs), shared with the native prebuild.

### Build everything

```bash
./build.sh vst                 # from the repo root: every phase, in order
./build.sh vst turnip pack     # just these phases
```

(`./build.sh vst` runs `vsthost_lib/scripts/build-all.sh`; `build-all.sh
--help` lists the phases.)

First run takes ~60–90 minutes on a fast laptop (llvm-mingw is the
single largest step at ~30–45 min). Subsequent runs are minutes —
each underlying script skips work whose outputs already exist.

Total disk usage during build: ~10 GB intermediates, ~1 GB final
output in `src/main/{jniLibs,assets}/`.

### What gets built

By phase (`build-all.sh`), everything under `vsthost_lib/` unless noted:

| Phase | Scripts | Outputs |
|---|---|---|
| llvm | `setup-fex-pivot.sh` | `external/llvm-mingw/install/` (the llvm-mingw cross toolchain) |
| x11 | native CMake target `x11_runtime_libs` | `build/x11_ui/sysroot/` at the repo root: X11 client libs built from `3rd_party/x11`, which `./build.sh` stages into `:app`'s base APK |
| winedeps | `fetch-x11-libs.sh`, `build-android-libs.sh`, `build-gnutls-android.sh` | `toolchain/x11-headers/` (host X11 headers), `toolchain/wine-fonts/`, `toolchain/x11-libs/lib{freetype,png16}.so`, `toolchain/gnutls-android-arm64/` |
| wine | `build-wine-pe.sh`, `build-wine-android.sh` | `external/wine-upstream/build-*/`: wine 11.9 + `patches/wine`, PE and Unix sides |
| fex | `build-fex-pe.sh` | `external/fex-upstream/build-{arm64ec,wow64}/Bin/lib{arm64ecfex,wow64fex}.dll` |
| dxvk | `build-dxvk.sh` | `src/main/assets/dxvk/` |
| mesa | `build-libdrm-android.sh`, `build-mesa-zink.sh` | `toolchain/drm-android/`, `src/main/assets/mesa-zink-libs.tar.gz` |
| adrenotools | `build-adrenotools.sh` | `toolchain/adrenotools-libs/` (libadrenotools + hook libs) |
| turnip | `build-turnip-hal.sh`, `build-libdrm-android.sh`, `build-turnip-icd.sh`, `build-vulkan-loader.sh` (+ `build-lavapipe-android.sh` with an Android LLVM) | `src/main/assets/turnip-libs.tar.gz` |
| hosts | `build-vst-host.sh`, `build-vst3-host.sh`, `build-uihost-stub.sh` | `src/main/assets/{vst_host,vst_host_x86,vst3_host}.exe`, `uihost_stub_x{64,86}.dll` |
| pack | `pack-wine-fex.py`, `build-symbol-map.sh` | `src/main/jniLibs/arm64-v8a/` (wine's ELF side as `libwine_*.so`, plus the freetype/png, gnutls and adrenotools libs), `src/main/assets/wine/` (the PE side), `wine-fex-manifest.json`, `wine-fex-nls.tar.gz`, `wine-fonts/`; `build/wine-symbol-map.txt` and `build/wine-prune-report.json` (host-side only) |

## Wine patch workflow

The numbered `patches/wine/NNNN-*.patch` apply on top of the clean
`wine-11.9` tag. They're applied by `scripts/apply-wine-patches.sh`
(called by both `build-wine-android.sh` and `build-wine-pe.sh` before
configure). The helper resets the wine submodule, runs `git apply` for
each numbered patch in order, and aborts on first conflict.

To add a new patch:
1. Edit wine source in `external/wine-upstream/` directly (after the
   build scripts have applied existing patches).
2. `cd external/wine-upstream && git diff > ../../patches/wine/NNNN-my-fix.patch`
   (the next free number)
3. Re-run `bash scripts/apply-wine-patches.sh` to verify it applies
   cleanly from a clean wine tree.
4. Old hand-curated patches live in `patches/wine/archived/` as audit
   trail — don't add new patches there.

If a patch stops applying after a wine submodule bump:
- `git -C external/wine-upstream apply --3way patches/wine/NNNN-...patch`
  surfaces the conflict in standard 3-way merge markers.

## Common build issues

- **`configure: error: gnutls not found`** — `build-gnutls-android.sh`
  didn't run or its install dir is gone. Re-run; the wine build script
  reads `toolchain/gnutls-android-arm64/`.
- **`wine: cannot find libwine_*.so`** at runtime — `pack-wine-fex.py`
  didn't stage everything. Check that all upstream build steps
  completed (each produces .so / .dll files the packer reads from).
- **`llvm-mingw/bin/clang: No such file or directory`** during FEX-PE /
  wine-PE builds — run `bash scripts/setup-fex-pivot.sh`.
- **i686 builds fail with `CONTEXT has no member named 'Rip'`** —
  the offending VEH handler in `external/vst_host/vst_host.c` is guarded
  with `#ifdef _WIN64`; if you're seeing this, a stale checkout. `git
  pull` and rebuild.
