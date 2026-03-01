# Third-party dependencies (Git submodules)

All third-party code lives here as submodules. After cloning:

```bash
git submodule update --init --recursive
```

## Patches

Local modifications are stored as numbered patch files in `patches/<submodule>/`.
`build.sh` applies them automatically (skipping already-applied patches).

## LV2 Host Stack

| Submodule | Description |
|-----------|-------------|
| **serd** | RDF syntax library |
| **zix** | Portable C utility library |
| **sord** | RDF store |
| **lv2** | LV2 specification headers |
| **sratom** | LV2 atom serialization |
| **lilv** | LV2 host library (Android fork) |

## LV2 Plugins - Guitarix

| Submodule | Description |
|-----------|-------------|
| **GxPlugins.lv2.Android** | Guitarix amp/effect plugins (Android build) |
| **guitarix** | Guitarix source (X11 UIs) |
| **GxCabSim.lv2** | Cabinet simulator |

## LV2 Plugins - Neural / AI

| Submodule | Description |
|-----------|-------------|
| **neural-amp-modeler-lv2** | Neural Amp Modeler (NAM) |
| **aidadsp-lv2** | AIDA-X headless DSP |
| **AIDA-X** | AIDA-X with DPF GUI |
| **NeuralRack** | NeuralRack multi-model host |
| **eigen** | Linear algebra (NAM dependency) |

## LV2 Plugins - Brummer

| Submodule | Description |
|-----------|-------------|
| **CollisionDrive** | Overdrive pedal |
| **MetalTone** | Distortion pedal |
| **FatFrog.lv2** | Tube overdrive |
| **ImpulseLoader** | IR cab loader |
| **XDarkTerror** | Amp simulation |
| **XTinyTerror** | Amp simulation |
| **ModularAmpToolKit.lv2** | PreAmps, PowerAmps, impulse modules |

## LV2 Plugins - Varcain

| Submodule | Description |
|-----------|-------------|
| **doubletracker.lv2** | Guitar double-track emulation |

## Audio / Math

| Submodule | Description |
|-----------|-------------|
| **oboe** | Android audio I/O (Google) |
| **fftw3** | FFT library (codelets generated via OCaml genfft) |
| **libsndfile** | Audio file I/O |
| **expat** | XML parser |

## Graphics - X11/Cairo

| Submodule | Description |
|-----------|-------------|
| **mesa** | OpenGL (software rasterizer for LV2 UIs) |
| **x11/util-macros** | X.Org autotools macros |
| **x11/xorgproto** | X11 protocol headers |
| **x11/xtrans** | X11 transport layer |
| **x11/libXau** | X11 authorization |
| **x11/xcb-proto** | XCB protocol definitions |
| **x11/libxcb** | XCB library |
| **x11/libX11** | X11 client library |
| **x11/libXext** | X11 extensions |
| **x11/libXrender** | X Render extension |
| **x11/pixman** | Pixel manipulation |
| **x11/libpng** | PNG image library |
| **x11/cairo** | 2D graphics library |
