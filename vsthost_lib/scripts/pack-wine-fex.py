#!/usr/bin/env python3
"""
Packs the fex-pivot build outputs (native arm64 wine + ARM64X/i386 PE DLLs +
libarm64ecfex.dll / libwow64fex.dll) into the APK's jniLibs + assets.

ELF vs PE placement:
  The ELF side (wine loader, wineserver, aarch64-unix/*.so) is exec'd or
  dlopen'd, so it ships in jniLibs as lib*.so (the only names the package
  manager extracts to nativeLibraryDir): libwine_loader.so, libwine_server.so,
  libwine_unix_<name>.so. WineSetup.kt symlinks each to its wine path.

  PE files (every DLL/EXE/driver wine maps itself) ship under their REAL names
  in assets/wine/lib/wine/<arch>-windows/ and WineAssetInstaller extracts them
  into the wine root as read-only files. They only need execute + execmod on
  app_data_file, which the full flavor's targetSdk 28 SELinux domain grants.
  (They used to be disguised as lib/arm64-v8a/libwine_NNNN.so too — ~1.5k
  Windows binaries under .so names, which is what antivirus engines flagged.)

Input (from build scripts already run):
  external/wine-upstream/build-android-arm64/loader/wine
  external/wine-upstream/build-android-arm64/server/wineserver
  external/wine-upstream/build-android-arm64/dlls/<name>/<name>.so
  external/wine-upstream/build-android-arm64/dlls/<name>/{aarch64,i386}-windows/<name>.dll
  external/wine-upstream/build-android-arm64/programs/<name>/{aarch64,i386}-windows/<name>.exe
  external/fex-upstream/build-arm64ec/Bin/libarm64ecfex.dll
  external/fex-upstream/build-wow64/Bin/libwow64fex.dll

Output:
  src/main/jniLibs/arm64-v8a/libwine_*.so      (ELF only)
  src/main/assets/wine/lib/wine/*-windows/*     (PE, real names)
  src/main/assets/wine-fex-manifest.json        (schema 2: per-entry kind,
                                                 size, sha256 + pack_digest)
"""

import argparse
import gzip
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path
from typing import Iterator

import wine_prune


# --- paths on device (inside app's files dir) ----------------------------
# These are the "chroot-style" paths the wine install will appear at on the
# device. They're not real chroot paths (no proot in this stack) — they're
# just the paths that WineSetup will symlink to. Wine env vars point here.
WINE_ROOT_DEVICE = "/wine"


def is_elf(path: Path) -> bool:
    try:
        return path.read_bytes()[:4] == b"\x7fELF" if path.stat().st_size >= 4 else False
    except OSError:
        return False


def is_pe(path: Path) -> bool:
    try:
        return path.read_bytes()[:2] == b"MZ" if path.stat().st_size >= 2 else False
    except OSError:
        return False


def file_needs_exec(path: Path) -> bool:
    return is_elf(path) or is_pe(path)


def iter_build_outputs(build_root: Path, fex_arm64ec: Path, fex_wow64: Path) -> Iterator[tuple[Path, str]]:
    """Yield (source_path, device_path) tuples for every file we ship."""
    # wine Unix-side binaries
    yield build_root / "loader/wine",            f"{WINE_ROOT_DEVICE}/bin/wine"
    # wine-preloader is DELIBERATELY omitted. wine's loader_exec tries
    # `<wineloader>-preloader` first; if that ENOENTs, it falls through to a
    # direct re-exec of wineloader itself. The preloader binary is linked at
    # a fixed virtual address (`-Wl,-Ttext=0x7d400000`) and exits silently
    # under Android's strict ASLR, leaving wine as a zombie with no output.
    # Wine runs fine without the preloader; we lose some memory-layout
    # optimisations the preloader was supposed to set up.
    yield build_root / "server/wineserver",      f"{WINE_ROOT_DEVICE}/bin/wineserver"

    # wine *.so unix-side dll bridges (in dlls/<name>/<name>.so)
    for so in sorted((build_root / "dlls").glob("*/*.so")):
        # Skip anything that's not at the dll's top level (avoid e.g. tests subdir leftovers).
        if so.parent.parent != build_root / "dlls":
            continue
        # Skip x86_64-windows builds (we don't enable that arch; this protects us if it ever appears).
        if "x86_64-windows" in so.parts or "i386-windows" in so.parts:
            continue
        yield so, f"{WINE_ROOT_DEVICE}/lib/wine/aarch64-unix/{so.name}"

    # ARM64X PE DLLs (the actual wine PE side, hybrid aarch64+arm64ec).
    # Match .dll, .drv (display/sys drivers), and .sys (kernel-style).
    for ext in ("*.dll", "*.drv", "*.sys"):
        for f in sorted((build_root / "dlls").glob(f"*/aarch64-windows/{ext}")):
            yield f, f"{WINE_ROOT_DEVICE}/lib/wine/aarch64-windows/{f.name}"
    for tlb in sorted((build_root / "dlls").glob("*/aarch64-windows/*.tlb")):
        yield tlb, f"{WINE_ROOT_DEVICE}/lib/wine/aarch64-windows/{tlb.name}"
    for exe in sorted((build_root / "programs").glob("*/aarch64-windows/*.exe")):
        yield exe, f"{WINE_ROOT_DEVICE}/lib/wine/aarch64-windows/{exe.name}"

    # i386 PE DLLs (32-bit WoW64 side, used when wine sees a PE32 binary).
    for ext in ("*.dll", "*.drv", "*.sys"):
        for f in sorted((build_root / "dlls").glob(f"*/i386-windows/{ext}")):
            yield f, f"{WINE_ROOT_DEVICE}/lib/wine/i386-windows/{f.name}"
    for tlb in sorted((build_root / "dlls").glob("*/i386-windows/*.tlb")):
        yield tlb, f"{WINE_ROOT_DEVICE}/lib/wine/i386-windows/{tlb.name}"
    for exe in sorted((build_root / "programs").glob("*/i386-windows/*.exe")):
        yield exe, f"{WINE_ROOT_DEVICE}/lib/wine/i386-windows/{exe.name}"

    # FEX PE DLLs (loaded by wine at runtime when it sees x86 / x86_64 code).
    yield fex_arm64ec, f"{WINE_ROOT_DEVICE}/lib/wine/aarch64-windows/libarm64ecfex.dll"
    yield fex_wow64,   f"{WINE_ROOT_DEVICE}/lib/wine/aarch64-windows/libwow64fex.dll"

    # X11 client libs are now SOURCE-BUILT by the native cmake X11 sysroot and
    # staged into the app module's jniLibs by build.sh (libX11/libxcb/libXau +
    # libXext/libXrender + libXi/libXfixes/libXrandr/libXcursor/libXxf86vm/
    # libXdmcp — all unversioned SONAMEs, XKB enabled). The merged APK exposes
    # them in nativeLibraryDir, so winex11.drv links + dlopens them at runtime.
    # We no longer ship Termux X11 prebuilts here, and libandroid-support is
    # dropped (the clean NDK X11 build doesn't need that Termux POSIX shim).
    # Only the FreeType chain stays here — wine's gdi32/win32u link
    # libfreetype.so + libpng16, both built from upstream source against the
    # NDK (scripts/build-android-libs.sh), staged under toolchain/x11-libs.
    repo_root = build_root.parent.parent.parent
    x11_lib_dir = repo_root / "toolchain/x11-libs"
    for so_name in [
        "libfreetype.so", "libpng16.so",
    ]:
        yield x11_lib_dir / so_name, f"_X11_RAW_/{so_name}"

    # GnuTLS for wine's secur32 (Schannel TLS). Built by
    # scripts/build-gnutls-android.sh into toolchain/gnutls-android-arm64/lib.
    # Ships under its original SONAME so wine's runtime link picks it up.
    yield repo_root / "toolchain/gnutls-android-arm64/lib/libgnutls.so", "_X11_RAW_/libgnutls.so"

    # libadrenotools + the namespace-bypass hook libs it needs (built by
    # scripts/build-adrenotools.sh into toolchain/adrenotools-libs). These are
    # the Winlator-style hook that loads Turnip as an Android-HAL GPU driver
    # (vulkan.ad07xx.so → /dev/kgsl). win32u/vulkan.c + the mesa vkshim dlopen
    # libadrenotools.so by name and adrenotools loads the hook libs by soname
    # from the APK nativeLibraryDir, so they ship under their real names.
    # Without them the adrenotools path can't load → GL editors render black.
    # libfile_redirect_hook.so / libgsl_alloc_hook.so are deliberately NOT
    # shipped: hook_impl only loads them for ADRENOTOOLS_DRIVER_FILE_REDIRECT /
    # _GPU_MAPPING_IMPORT, and every adrenotools_open_libvulkan() caller (wine
    # patch 0030, mesashim/vulkan_turnip_shim.c, ahbspike) passes featureFlags
    # 1 (CUSTOM) or 0. Dead libc/driver-symbol interposers only add AV-scanner
    # noise. (They stay in the stale-lib cleanup below so re-packs remove them.)
    adreno_lib_dir = repo_root / "toolchain/adrenotools-libs"
    for so_name in [
        "libadrenotools.so", "libhook_impl.so", "libmain_hook.so",
    ]:
        yield adreno_lib_dir / so_name, f"_X11_RAW_/{so_name}"


def strip_symbol_versions(path: Path) -> None:
    """Zero the DT_VERSYM / DT_VERNEED / DT_VERNEEDNUM dynamic-section
    entries so Bionic's loader bypasses glibc-style symbol-version
    matching for this lib. Termux libfreetype/libpng16/libz declare a
    verneed (ZLIB_1.2.3.4 etc.) that Bionic mis-parses ("cannot find
    'Export' from verneed[0]"). Stripping the *sections* with objcopy
    leaves the dynamic tags pointing at garbage — Bionic still tries
    to parse them and now fails with "unsupported verneed[0] vn_version:
    0". We have to neutralise the tags themselves.
    Used only on the X11/freetype/zlib libs we ship — wine's own .so
    binaries don't carry verneed."""
    import struct
    DT_NULL          = 0
    DT_VERSYM        = 0x6ffffff0
    DT_VERDEF        = 0x6ffffffc
    DT_VERDEFNUM     = 0x6ffffffd
    DT_VERNEED       = 0x6ffffffe
    DT_VERNEEDNUM    = 0x6fffffff
    NEUTRALISE = {DT_VERSYM, DT_VERDEF, DT_VERDEFNUM, DT_VERNEED, DT_VERNEEDNUM}

    with open(path, "rb+") as f:
        data = bytearray(f.read())

        # ELF64 only (we only ship aarch64 here).
        if data[:4] != b"\x7fELF" or data[4] != 2:
            return
        # Find .dynamic via program header PT_DYNAMIC (tag 2).
        e_phoff   = struct.unpack_from("<Q", data, 0x20)[0]
        e_phentsz = struct.unpack_from("<H", data, 0x36)[0]
        e_phnum   = struct.unpack_from("<H", data, 0x38)[0]
        dyn_off, dyn_sz = 0, 0
        for i in range(e_phnum):
            base = e_phoff + i * e_phentsz
            p_type = struct.unpack_from("<I", data, base)[0]
            if p_type == 2:  # PT_DYNAMIC
                dyn_off = struct.unpack_from("<Q", data, base + 8)[0]
                dyn_sz  = struct.unpack_from("<Q", data, base + 32)[0]
                break
        if not dyn_sz:
            return

        # Walk Elf64_Dyn entries; zero each NEUTRALISE tag's d_tag (turns
        # it into a stale DT_NULL-like entry that Bionic skips).
        for off in range(dyn_off, dyn_off + dyn_sz, 16):
            d_tag = struct.unpack_from("<Q", data, off)[0]
            if d_tag == DT_NULL:
                break
            if d_tag in NEUTRALISE:
                # Replace with DT_DEBUG (21) + d_val=0; harmless for runtime.
                struct.pack_into("<Q", data, off, 21)
                struct.pack_into("<Q", data, off + 8, 0)

        f.seek(0)
        f.write(data)
        f.truncate()


def run_checked(cmd: list[str]) -> str:
    """Run cmd and return its stdout; exit with its stderr if it fails."""
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"error: {' '.join(cmd)} failed ({r.returncode}):\n{r.stderr.strip()}")
    return r.stdout


def normalize_sonames(path: Path) -> None:
    """patchelf the file in place: strip version suffixes from SONAME and
    NEEDED entries (libz.so.1 → libz.so, libbz2.so.1.0 → libbz2.so).
    Bionic's dynamic linker rejects versioned SONAMEs from APKs."""
    # readelf to enumerate NEEDED + SONAME
    out = run_checked(["readelf", "-d", str(path)])

    def strip_ver(name: str) -> str:
        # libfoo.so.1 → libfoo.so ; libfoo.so.1.2 → libfoo.so
        i = name.find(".so.")
        if i < 0:
            return name
        return name[: i + 3]

    for line in out.splitlines():
        line = line.strip()
        # e.g. "0x00…(NEEDED) Shared library: [libz.so.1]"
        if "(NEEDED)" in line and "[" in line:
            orig = line.split("[", 1)[1].rstrip("]")
            normalized = strip_ver(orig)
            if normalized != orig:
                run_checked(["patchelf", "--replace-needed", orig, normalized, str(path)])
        elif "(SONAME)" in line and "[" in line:
            orig = line.split("[", 1)[1].rstrip("]")
            normalized = strip_ver(orig)
            if normalized != orig:
                run_checked(["patchelf", "--set-soname", normalized, str(path)])


def strip_into(src: Path, dst: Path, strip_tool: str) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    # llvm-strip handles both the ELF and the PE (COFF) files we ship.
    run_checked([strip_tool, "--strip-unneeded", str(dst)])


def elf_lib_name(device_path: str) -> str:
    """jniLibs name for a wine ELF: descriptive, and a lib*.so name the
    package manager will extract to nativeLibraryDir."""
    if device_path == f"{WINE_ROOT_DEVICE}/bin/wine":
        lib = "libwine_loader.so"
    elif device_path == f"{WINE_ROOT_DEVICE}/bin/wineserver":
        lib = "libwine_server.so"
    elif "/aarch64-unix/" in device_path:
        stem = Path(device_path).name.removesuffix(".so")
        lib = "libwine_unix_" + re.sub(r"[^A-Za-z0-9_]", "_", stem) + ".so"
    else:
        raise ValueError(f"no jniLibs name rule for ELF {device_path}")
    if not re.fullmatch(r"lib[A-Za-z0-9_]+\.so", lib):
        raise ValueError(f"bad jniLibs name {lib} for {device_path}")
    return lib


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def toolchain_prop(key: str) -> str:
    """The value of <key> in config/toolchain.properties (repository root)."""
    props = Path(__file__).resolve().parents[2] / "config" / "toolchain.properties"
    for line in props.read_text().splitlines():
        name, sep, value = line.partition("=")
        if sep and name.strip() == key:
            return value.strip()
    raise SystemExit(f"{props}: no {key}")


def main() -> int:
    # Default NDK strip path: $ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
    # If $ANDROID_NDK isn't set, fall back to ndk.version.vst (config/toolchain.properties)
    # under $HOME/Android/Sdk/ - the same default the wine + FEX build scripts use
    # (scripts/lib/common.sh). Override with --strip <path>.
    default_ndk = os.environ.get("ANDROID_NDK") or os.path.expanduser(
        f"~/Android/Sdk/ndk/{toolchain_prop('ndk.version.vst')}")
    default_strip = f"{default_ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

    ap = argparse.ArgumentParser()
    ap.add_argument("--repo-root", required=True, type=Path)
    ap.add_argument("--strip", default=default_strip,
                    help="llvm-strip path (defaults to $ANDROID_NDK/.../llvm-strip)")
    ap.add_argument("--no-prune", action="store_true",
                    help="ship every wine PE file (ignore wine-prune.conf)")
    args = ap.parse_args()

    repo = args.repo_root.resolve()
    wine_build = repo / "external/wine-upstream/build-android-arm64"
    fex_arm64ec = repo / "external/fex-upstream/build-arm64ec/Bin/libarm64ecfex.dll"
    fex_wow64 = repo / "external/fex-upstream/build-wow64/Bin/libwow64fex.dll"
    out_jni = repo / "src/main/jniLibs/arm64-v8a"
    out_assets = repo / "src/main/assets"
    out_manifest = out_assets / "wine-fex-manifest.json"

    outputs = list(iter_build_outputs(wine_build, fex_arm64ec, fex_wow64))

    # Check every input and tool before touching the previous pack: a missing
    # or unexpected build output fails the pack instead of being left out.
    errors = [f"missing input: {src}" for src, _ in outputs if not src.exists()]
    errors += [f"input is neither ELF nor PE: {src}" for src, _ in outputs
               if src.exists() and not file_needs_exec(src)]
    errors += [f"tool not found: {t}" for t in (args.strip, "readelf", "patchelf")
               if not shutil.which(t)]
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1

    out_jni.mkdir(parents=True, exist_ok=True)

    # Wipe the previous pack's wine outputs (incl. the old hex-numbered
    # libwine_fNNN.so names) so a re-pack never leaves stale files behind.
    for stale in out_jni.glob("libwine_*.so"):
        stale.unlink()
    shutil.rmtree(out_assets / "wine", ignore_errors=True)

    entries: list[dict] = []
    elf_names: set[str] = set()

    # Also wipe any prior X11/gnutls/freetype/png libs we shipped directly
    # under their original names so a re-pack stays clean.
    for stale_name in ("libX11.so", "libXau.so", "libxcb.so", "libXdmcp.so",
                       "libXext.so", "libXrender.so", "libXi.so", "libXfixes.so",
                       "libXrandr.so", "libXcursor.so", "libXxf86vm.so",
                       "libandroid-support.so",
                       "libfreetype.so", "libpng16.so",
                       "libgnutls.so",
                       "libadrenotools.so", "libhook_impl.so", "libmain_hook.so",
                       "libfile_redirect_hook.so", "libgsl_alloc_hook.so"):
        stale = out_jni / stale_name
        if stale.exists():
            stale.unlink()

    # Leave unneeded wine PE files out (wine-prune.conf); refuses to prune
    # anything a kept module still depends on.
    pruned: set[str] = set()
    if not args.no_prune:
        pe_files = [(src, dp) for src, dp in outputs
                    if not dp.startswith("_X11_RAW_/") and is_pe(src)]
        try:
            result = wine_prune.plan(pe_files, repo / "wine-prune.conf", wine_prune.find_readobj(repo))
        except wine_prune.PruneError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        pruned = result["pruned"]
        report = repo / "build/wine-prune-report.json"
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps(result["report"], indent=1))
        print(f"pruned {len(pruned)} of {len(pe_files)} wine PE files (report → {report})")
        for p in result["report"]["deny_patterns_matching_nothing"]:
            print(f"  WARN: wine-prune.conf deny pattern matches nothing: {p}", file=sys.stderr)
    # A unix-side lib whose PE module is pruned in every arch is dead weight too.
    pruned_stems = {Path(dp).stem.lower() for dp in pruned}
    kept_stems = {Path(dp).stem.lower() for src, dp in outputs
                  if "-windows/" in dp and dp not in pruned}
    orphan_stems = pruned_stems - kept_stems

    total_bytes = 0
    for src, device_path in outputs:
        if device_path in pruned or (
                "/aarch64-unix/" in device_path and Path(device_path).stem.lower() in orphan_stems):
            continue
        # X11 libs ship with their original SONAME so Bionic's linker can
        # resolve them by the names baked into winex11.so / libX11.so etc.
        # Marker prefix tells us not to rename + not to put them in the
        # manifest (WineSetup doesn't symlink them; they live straight in
        # nativeLibraryDir where the dynamic linker auto-searches).
        if device_path.startswith("_X11_RAW_/"):
            real_name = device_path.removeprefix("_X11_RAW_/")
            dst = out_jni / real_name
            strip_into(src, dst, args.strip)
            # X11 libs from Termux .debs still need SONAME normalisation
            # (libxcb.so.1 → libxcb.so etc) because Bionic only accepts
            # unversioned names from nativeLibraryDir. Freetype/png are
            # built clean by scripts/build-android-libs.sh so they
            # already have unversioned SONAMEs — patchelf is a no-op
            # for them.
            normalize_sonames(dst)
            total_bytes += dst.stat().st_size
            continue

        if is_pe(src):
            # PE: real name under assets/wine/<path below the wine root>.
            asset = "wine/" + device_path.removeprefix(f"{WINE_ROOT_DEVICE}/")
            dst = out_assets / asset
            entry = {"kind": "asset", "path": device_path, "asset": asset}
        else:
            lib_name = elf_lib_name(device_path)
            if lib_name in elf_names:
                raise ValueError(f"jniLibs name collision: {lib_name} ({device_path})")
            elf_names.add(lib_name)
            dst = out_jni / lib_name
            entry = {"kind": "elf", "path": device_path, "lib": lib_name}
        strip_into(src, dst, args.strip)
        entry["size"] = dst.stat().st_size
        entry["sha256"] = sha256_of(dst)
        total_bytes += entry["size"]
        entries.append(entry)

    # Digest over every entry, so the app re-installs exactly when the pack changes.
    pack_digest = hashlib.sha256("\n".join(sorted(
        f"{e['kind']} {e['path']} {e['size']} {e['sha256']} {e.get('lib') or e.get('asset')}"
        for e in entries)).encode()).hexdigest()
    with out_manifest.open("w") as f:
        json.dump({
            "schema": 2,
            "wine_root_device": WINE_ROOT_DEVICE,
            "pack_digest": pack_digest,
            "entries": entries,
        }, f, indent=2)

    n_elf = sum(e["kind"] == "elf" for e in entries)
    print(f"packed {n_elf} ELF libs → {out_jni}, {len(entries) - n_elf} PE files → "
          f"{out_assets / 'wine'} ({total_bytes/1024/1024:.1f} MB total)")
    print(f"manifest → {out_manifest} (pack_digest {pack_digest[:16]}…)")

    # --- wine NLS tarball -----------------------------------------------------
    # WineSetup.kt extracts this into <wineRoot>/share/wine at runtime.
    # The .nls files are wine's codepage conversion tables; without them
    # plugins doing MultiByteToWideChar (most do) crash on first use.
    wine_src_nls = repo / "external/wine-upstream/nls"
    out_nls_tar = repo / "src/main/assets/wine-fex-nls.tar.gz"
    if wine_src_nls.exists():
        # Deterministic archive (sorted entries, no owners/mtimes, gzip mtime
        # 0) so the APK content doesn't change between identical builds.
        def normalized(ti: tarfile.TarInfo) -> tarfile.TarInfo:
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = ""
            ti.mtime = 0
            ti.mode = 0o755 if ti.isdir() else 0o644
            return ti
        with gzip.GzipFile(out_nls_tar, "wb", mtime=0) as gz, \
                tarfile.open(fileobj=gz, mode="w", format=tarfile.USTAR_FORMAT) as tf:
            tf.add(wine_src_nls, arcname="nls", recursive=False, filter=normalized)
            for f in sorted(wine_src_nls.iterdir()):
                if f.is_file() and f.name.endswith(".nls"):
                    tf.add(f, arcname=f"nls/{f.name}", filter=normalized)
        print(f"NLS tarball → {out_nls_tar} ({out_nls_tar.stat().st_size/1024:.0f} KB)")
    else:
        print(f"error: wine NLS source dir not found at {wine_src_nls}", file=sys.stderr)
        return 1

    # --- wine fonts ----------------------------------------------------------
    # fetch-x11-libs.sh collects Liberation + DejaVu TTFs into toolchain/
    # wine-fonts/. Stage them into assets/wine-fonts/ for WineSetup.kt's
    # seedFonts() to copy into each wineprefix's drive_c/windows/Fonts.
    src_fonts = repo / "toolchain/wine-fonts"
    out_fonts = repo / "src/main/assets/wine-fonts"
    out_fonts.mkdir(parents=True, exist_ok=True)
    if src_fonts.exists():
        # Wipe stale fonts so renames upstream don't leave orphans.
        for stale in out_fonts.glob("*.ttf"):
            stale.unlink()
        count = 0
        for ttf in src_fonts.glob("*.ttf"):
            shutil.copy2(ttf, out_fonts / ttf.name)
            count += 1
        print(f"fonts → {out_fonts} ({count} files)")
    else:
        print(f"error: toolchain/wine-fonts not found; run fetch-x11-libs.sh first", file=sys.stderr)
        return 1

    # --- wine's own built-in core fonts --------------------------------------
    # Stock wine ships real Windows core faces (Tahoma, System, MS Sans Serif,
    # Symbol, Marlett, Wingdings, Webdings, Courier, Fixedsys, Small Fonts) as
    # pre-built .ttf in its source fonts/ dir. These are what makes a normal
    # wine install render CEF/Chromium UIs out of the box: wine's user32 system
    # metrics return "MS Shell Dlg"/"System", which resolve to real Tahoma/
    # System here, so CEF's gfx::win::SystemFonts CreateSkTypeface() succeeds
    # and never enters its infinite GetSystemFont re-entry (the FEX
    # manually-mapped-region miscompile that black-screened BIAS FX 2's editor).
    # They are upstream files (survive build-wine-android.sh's source reset), so
    # copy them straight from the wine tree. WineSetup.seedFontRegistry()
    # registers them full-path for DirectWrite.
    wine_core_fonts = [
        "tahoma.ttf", "tahomabd.ttf", "system.ttf", "ms_sans_serif.ttf",
        "symbol.ttf", "marlett.ttf", "wingding.ttf", "webdings.ttf",
        "courier.ttf", "fixedsys.ttf", "small_fonts.ttf",
    ]
    wine_fonts_src = repo / "external/wine-upstream/fonts"
    if wine_fonts_src.exists():
        wc = 0
        for name in wine_core_fonts:
            f = wine_fonts_src / name
            if f.exists():
                shutil.copy2(f, out_fonts / name)
                wc += 1
            else:
                print(f"error: wine core font missing: {f}", file=sys.stderr)
                return 1
        print(f"wine core fonts → {out_fonts} ({wc} files)")
    else:
        print(f"error: wine source fonts dir not found at {wine_fonts_src}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
