# vsthost_lib

In-process Windows VST2/VST3 plugin host for GuitarRackCraft. Built on
native wine-arm64ec + FEX-Emu for x86_64 plugin translation. Ships as
an Android library module consumed by `:app`'s `full` flavor (sideload
distribution, targetSdk=28). The `playstore` flavor (targetSdk=35)
doesn't depend on this module — see [`targetsdk35-blocked`](../docs/)
for why the SDK 29+ W^X cliff blocks wine.

## Architecture (brief)

- **`src/main/cpp/vst/{VstFactory,WineVstPlugin}.cpp`** — `IPlugin` bridge
  consumed by `:app` via prefab. Forks one wine subprocess per imported
  VST; audio flows via SysV shm rings.
- **`src/main/cpp/x11/`** — minimal in-process X11 server. Wine's
  winex11.drv connects to it over a TCP loopback socket; output is
  blitted to a Compose `SurfaceView` via EGL/GLES2.
- **`src/main/cpp/launcher/WineHostProcess.cpp`** — fork+exec wine,
  wired through `vst_host.exe` (PE32+) which loads the user's plugin DLL.
- **`external/{wine-upstream,fex-upstream,llvm-mingw}`** — git submodules.
- **`patches/wine/*.patch`** — Bionic / FEX-pivot adaptations applied to
  the wine submodule before configure.
- **`scripts/`** — build pipeline (see below).

## Build from sources

Everything is source-built. There are no committed binaries in this
module; `src/main/{jniLibs,assets}/` are gitignored and populated by the
build pipeline.

### One-time host setup

```bash
# 1. Android NDK r26.1
export ANDROID_NDK=$HOME/Android/Sdk/ndk/26.1.10909125
# (or download from https://developer.android.com/ndk/downloads)

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
git submodule update --init --recursive vsthost_lib/external/wine-upstream \
                                        vsthost_lib/external/fex-upstream \
                                        vsthost_lib/external/llvm-mingw
```

This pulls:
- **wine** at tag `wine-10.10` (~150 MB)
- **FEX-Emu** at commit `07f7aa3c8` (~50 MB)
- **llvm-mingw** at tag `20250730` (~13 MB)

### Build everything

```bash
cd vsthost_lib
bash scripts/build-all.sh
```

First run takes ~60–90 minutes on a fast laptop (llvm-mingw is the
single largest step at ~30–45 min). Subsequent runs are minutes —
each underlying script skips work whose outputs already exist.

Total disk usage during build: ~10 GB intermediates, ~1 GB final
output in `src/main/{jniLibs,assets}/`.

### What gets built

| Output | Source | Step |
|---|---|---|
| `external/llvm-mingw/install/bin/*` | mstorsjo/llvm-mingw @ 20250730 | setup-fex-pivot |
| `toolchain/x11-{headers,libs}/` | Termux .deb packages | fetch-x11-libs |
| `toolchain/x11-libs/libpng16.so` | libpng upstream | build-android-libs |
| `toolchain/x11-libs/libfreetype.so` | freetype upstream | build-android-libs |
| `toolchain/gnutls-android-arm64/lib/libgnutls.so` | gnutls upstream | build-gnutls-android |
| `external/wine-upstream/build-arm64ec/dlls/**/*.dll` | wine 10.10 + patches | build-wine-pe |
| `external/wine-upstream/build-android-arm64/{loader,server}/*` | wine 10.10 + patches | build-wine-android |
| `external/fex-upstream/build-{arm64ec,wow64}/Bin/lib*.dll` | FEX-Emu | build-fex-pe |
| `src/main/assets/vst_host.exe` | external/vst_host/vst_host.c | build-vst-host |
| `src/main/assets/vst_host_x86.exe` | same | build-vst-host |
| `src/main/assets/uihost_stub_x{64,86}.dll` | external/uihost_stub/ | build-uihost-stub |
| `src/main/jniLibs/arm64-v8a/lib*.so` (~1500 files) | all of above, packed | pack-wine-fex |
| `src/main/assets/wine-fex-manifest.json` | manifest | pack-wine-fex |

## Wine patch workflow

`patches/wine/0001..0007*.patch` apply on top of the clean
`wine-10.10` tag. They're applied by `scripts/apply-wine-patches.sh`
(called by both `build-wine-android.sh` and `build-wine-pe.sh` before
configure). The helper resets the wine submodule, runs `git apply` for
each numbered patch in order, and aborts on first conflict.

To add a new patch:
1. Edit wine source in `external/wine-upstream/` directly (after the
   build scripts have applied existing patches).
2. `cd external/wine-upstream && git diff > ../../patches/wine/0008-my-fix.patch`
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
