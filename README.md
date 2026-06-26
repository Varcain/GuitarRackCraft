# Guitar RackCraft

[![CI](https://github.com/Varcain/GuitarRackCraft/actions/workflows/ci.yml/badge.svg)](https://github.com/Varcain/GuitarRackCraft/actions/workflows/ci.yml)

A real-time guitar effects processor for Android. Hosts 120+ LV2 audio plugins in a chainable rack interface with low-latency audio via Oboe and native plugin UIs rendered through a custom X11/EGL emulation layer.

It also includes **experimental** support for hosting Windows VST2/VST3 plugins (x86/x64) directly on-device via Wine + FEX emulation (see [Windows VST plugins](#windows-vst-plugins)).

![Screenshot](screenshot.png)

## Features

- Chain multiple LV2 plugins with drag-and-drop reordering
- Real-time audio processing with low-latency Oboe I/O
- Native X11 plugin UIs rendered on Android via custom X11 server + EGL
- Host Windows VST2/VST3 plugins (x86/x64) via Wine + FEX emulation - see [Windows VST plugins](#windows-vst-plugins)
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

## Windows VST plugins

In addition to the bundled LV2 plugins, the **full** flavor can host **Windows VST2/VST3**
plugins (32-bit x86 and 64-bit x64) directly on Android - no PC required. Each plugin runs inside
a bundled Windows compatibility layer:

- **[Wine](https://www.winehq.org/)** provides the Win32 API and PE loader, so the plugin's `.dll` / `.vst3` loads unmodified.
- **[FEX-Emu](https://fex-emu.com/)** JIT-translates the plugin's x86/x64 machine code to ARM64.
- The plugin editor is bridged through the same custom X11 server onto an Android surface; **DXVK → Turnip** (Mesa Vulkan on Adreno) translates Direct3D 11 plugin GUIs.

Import a plugin with the in-app VST manager (point it at a `.dll` or `.vst3`). Imported plugins
appear under the **Windows VST** group in the plugin browser - tagged with format (VST2/VST3) and
architecture (x86/x64) badges - and chain alongside LV2 plugins like any other effect.

> Windows VSTs run under emulation, so they use more CPU than native LV2 plugins, and plugins that
> require online or hardware DRM activation may not work. The Windows VST host is built only in the
> `full` flavor (`HAS_VST_HOST=true`); the `playstore` flavor omits it.

## License

GPLv3 - see [LICENSE](LICENSE).
