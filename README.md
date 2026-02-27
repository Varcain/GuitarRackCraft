# Guitar RackCraft

[![CI](https://github.com/Varcain/GuitarRackCraft/actions/workflows/ci.yml/badge.svg)](https://github.com/Varcain/GuitarRackCraft/actions/workflows/ci.yml)

A real-time guitar effects processor for Android. Hosts 120+ LV2 audio plugins in a chainable rack interface with low-latency audio via Oboe and native plugin UIs rendered through a custom X11/EGL emulation layer.

![Screenshot](screenshot.png)

## Features

- Chain multiple LV2 plugins with drag-and-drop reordering
- Real-time audio processing with low-latency Oboe I/O
- Native X11 plugin UIs rendered on Android via custom X11 server + EGL
- Neural amp modeling (NAM, AIDA-X)
- WAV file playback through the effects chain
- Audio recording (raw input + processed output)
- Preset save/restore

## Requirements

- Android 8.0+ (API 26), arm64-v8a
- JDK 17
- Android SDK (API 35, NDK 27.2.12479018, CMake 3.22.1)
- System packages: `ninja-build meson python3 python3-mako pkg-config autoconf automake libtool gettext patch cmake flex bison ocaml ocamlbuild ocaml-findlib libnum-ocaml-dev`

## Build

```bash
# Initialize submodules
git submodule update --init --recursive

# Build native libraries
./build.sh

# Build and install debug APK
./run.sh debug

# Build release APK + AAB
./run.sh release

# Build Play Store AAB with asset packs
./run.sh playstore
```

### Build flavors

| Flavor | Description |
|--------|-------------|
| **full** | All plugins bundled in a single APK |
| **playstore** | Plugins split into asset packs (gxplugins, neural, brummer) for Play Store delivery |

## Architecture

- **Kotlin/Compose** - Android UI layer
- **C++17** - Audio engine, LV2 host, X11 server
- **Oboe** - Low-latency audio I/O
- **lilv** - LV2 plugin loading and management
- **Cairo/Mesa** - 2D/3D rendering for plugin UIs
- **X11 emulation** - Custom minimal X11 server bridging native plugin UIs to Android surfaces

See [3rd_party/README.md](3rd_party/README.md) for the full list of dependencies.

## License

GPLv3 - see [LICENSE](LICENSE).
