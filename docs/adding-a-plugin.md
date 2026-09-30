# Adding an LV2 plugin

A bundled LV2 plugin is built by the native prebuild (`cmake/`, driven by
`build.sh`), staged into the app and the Play asset packs, and discovered at
runtime by lilv. Adding one takes five things; the rest is derived from them.

| What | Where |
|------|-------|
| The source, as a submodule (plus local patches) | `3rd_party/<Name>`, `3rd_party/patches/<Name>/NNNN-*.patch` |
| A target file that builds and stages it | `cmake/targets/<name>.cmake` |
| One registry line | `cmake/plugins.cmake` |
| A description for the plugin browser | `plugin_descriptions.json` |
| A credit | `3rd_party/README.md` |

Derived from the registry, with nothing else to edit: which target files
CMake includes, `all_plugins`, what the metadata step waits for and which
bundles it describes, which Play asset pack the libraries ship in, and the
staging into `app/src/` (`build.sh stage`). `build.sh clean` removes all of
`build/` except `build/gradle`, so a new build directory needs no clean entry.

## 1. Submodule and patches

```bash
git submodule add <repo-url> 3rd_party/<Name>
```

Local changes go in numbered `patch -p1` diffs under
`3rd_party/patches/<Name>/`. `build.sh` (`./build.sh patch`) applies them in
name order through `scripts/apply-patches.sh`: a patch that is already in the
tree is skipped, and one that neither applies nor is applied stops the build.

## 2. The target file: `cmake/targets/<name>.cmake`

`<name>` is the registry name (lower case by convention). The file must:

1. stage the plugin's TTLs into `${ASSETS_DIR}/<Bundle>.lv2/`,
2. build the DSP `.so` (and the UI `.so`, if any),
3. stage the libraries into `${JNILIBS_DIR}` as `lib<binary>.so`, where
   `<binary>` is the name in the TTL's `lv2:binary` / `ui:binary`,
4. define a target `<name>_done` that depends on all of it.

`JNILIBS_DIR` is set per plugin by `cmake/CMakeLists.txt`, to
`build/prebuild/stage/<PACK>`; don't set it yourself. CMake fails at configure
time if `<name>_done` is missing.

Pick the closest existing file as a template:

| Plugin shape | Template | Helpers it uses |
|--------------|----------|-----------------|
| brummer10-style DSP + xputty/cairo UI | `collisiondrive.cmake`, `fatfrog.cmake` | `lv2_stage_bundle`, `brummer_add_plugin` |
| Custom DSP build (extra libraries) + xputty UI | `neuralrack.cmake`, `impulseloader.cmake` | `lv2_stage_bundle`, `brummer_setup_xputty`, `brummer_add_ui_target`, `lv2_sync_dsp_ui` |
| Several plugins from one repository | `modamptk.cmake` | as above, once per plugin |
| Upstream CMake project (ExternalProject) | `nam.cmake`, `aidax.cmake` | `lv2_sync_to_jnilibs` on the build's `.so` |
| DSP only, generated TTLs | `doubletracker.cmake` | hand-written sync |

The helpers live in `cmake/modules/`:

- `lv2_stage_bundle(<bundle_dir> TTL_DIR <dir> [TTLS <file>...] [MOD_DIR <dir>])`
  (`LV2PluginUtils.cmake`) copies `manifest.ttl` and the listed TTLs, which
  must exist, plus MOD extras when present: MOD's `manifest.ttl` (it adds
  `rdfs:seeAlso <modgui.ttl>`), `modgui.ttl` and `modgui/`.
- `lv2_set_dsp_properties(<target> <output_name> <build_dir>)` sets the DSP
  output name and the usual compile flags; `lv2_strip_and_save_debug` keeps an
  unstripped `<name>_debug.so` next to the build output.
- `lv2_sync_dsp_ui(NAME ... OUTPUT_NAME ... BUILD_DIR ... DSP_TARGET ... UI_TARGET ...)`
  copies and strips `<OUTPUT_NAME>.so` and `<OUTPUT_NAME>_ui.so` into
  `${JNILIBS_DIR}` and defines `<NAME>_done`.
- `lv2_sync_to_jnilibs(<target> <source_dir> <depends>)` does the same for
  every DSP `.so` under a directory (UI and `_debug` files are skipped by
  default); define `<name>_done` on top of it.
- `brummer_add_plugin(...)` (`BrummerPlugin.cmake`) is the whole DSP + UI +
  sync chain for the brummer10 layout.

Things that bite:

- DSP sources want `${LV2_COMPAT_DIR}` (redirects for old `lv2plug.in/ns/...`
  include paths) and `${LV2_INCLUDE}` (the LV2 headers) on their include path.
  `generate_lv2_compat_headers(<dir>)` makes a per-plugin copy if needed.
- `lv2_set_dsp_properties` compiles with `-std=c++17`. For a C DSP, add
  `target_compile_options(<target> PRIVATE -std=c11)` after it (the last flag
  wins).
- A cairo/X11 UI links `grc_x11_ui_libs` (cairo, pixman, png, the X11 client
  libraries, the MIT-SHM stub) and needs `add_dependencies(<ui> x11_sysroot)`.
- Name the libraries uniquely: two staged libraries with the same file name
  stop `build.sh`.

## 3. The registry line: `cmake/plugins.cmake`

```cmake
grc_plugin(<name> PACK <gx|neural|brummer> AUTHOR <author> BUNDLES <Bundle>...)
```

- `PACK`: the Play asset pack the libraries go to in the `playstore` flavor
  (the `full` flavor ships every plugin in its jniLibs). Use the one that
  matches the plugin's family; it only decides which install-time pack holds
  the files.
- `BUNDLES`: the plugin's own bundles, `assets/lv2/<Bundle>.lv2`, which the
  metadata step lists in `plugin_metadata.json` with `AUTHOR` as their author,
  so the plugin browser groups them. A declared bundle that isn't staged is a
  build error.

## 4. Description and credit

- `plugin_descriptions.json`: add `"<doap:name>": "<one line>"`, keyed by the
  plugin's `doap:name` from its TTL.
- `3rd_party/README.md`: add the submodule to its author's table.

## 5. Build and check

```bash
./build.sh native <name>_done     # configure (the new files trigger it), build, stage
./gradlew assembleFullDebug       # or ./run.sh debug
```

`./build.sh native <target>...` builds only what you name and then restages;
run a plain `./build.sh` (or `./build.sh native`) once before committing to
build everything and regenerate the metadata.

These fail loudly instead of shipping a broken plugin:

- a TTL `lv2_stage_bundle` expects is missing;
- a bundle declared in `BUNDLES` wasn't staged (metadata step);
- two staged libraries share a file name (`build.sh` staging);
- a library left in `app/src/main/jniLibs` that `config/core-libs.txt` doesn't
  allow, or a missing native prebuild (Gradle's `verifyNativeInputs<Variant>`);
- empty asset packs when bundling the `playstore` flavor (`verifyAssetPacks`).

## How it fits together at runtime

- `build.sh stage` mirrors `build/prebuild/stage/`: `core/` to
  `app/src/main/jniLibs` (the base APK), every pack to `app/src/full/jniLibs`
  (the `full` flavor), and each pack to its `<pack>/src/main/assets/plugins`
  (the `playstore` flavor, listed in `app/src/playstore/assets/plugin_libs.txt`
  for `PluginAssetExtractor`).
- The TTLs ship as assets under `assets/lv2/`. The app extracts the files
  listed in `assets/lv2_files.txt` (which `build.sh stage` regenerates), so a
  bundle missing from it is invisible.
- At startup `LV2PluginFactory::rewriteManifestPaths()` points each
  manifest's `lv2:binary <x.so>` at `lib<x.so>` in the app's native library
  dir (`full`), or in the directory the asset packs were extracted to
  (`playstore`).
