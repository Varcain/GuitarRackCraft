#!/usr/bin/env python3
"""
Packs the fex-pivot build outputs (native arm64 wine + ARM64X PE DLLs +
libarm64ecfex.dll / libwow64fex.dll) into the APK's jniLibs + assets.

Why the rename to libwine_NNNN.so:
  Android 10+ blocks mprotect(PROT_EXEC) on files in /data/data/<app>/files/.
  Only files in nativeLibraryDir (which Android extracts from the APK's
  lib/<abi>/) get an exec-allowed SELinux label for the app's UID. So
  every wine ELF and every PE DLL that wine will mmap with PROT_EXEC
  must end up in jniLibs and be named lib*.so. A manifest records the
  chroot-style path each lib*.so represents, and WineSetup.kt builds
  symlinks from the chroot path → its lib*.so on first run.

Input (from build scripts already run):
  external/wine-upstream/build-android-arm64/loader/wine
  external/wine-upstream/build-android-arm64/loader/wine-preloader
  external/wine-upstream/build-android-arm64/server/wineserver
  external/wine-upstream/build-android-arm64/dlls/<name>/<name>.so
  external/wine-upstream/build-android-arm64/dlls/<name>/aarch64-windows/<name>.dll
  external/fex-upstream/build-arm64ec/Bin/libarm64ecfex.dll
  external/fex-upstream/build-wow64/Bin/libwow64fex.dll

Output:
  src/main/jniLibs/arm64-v8a/libwine_NNNN.so
  src/main/assets/wine-fex-manifest.json
  (no wine-fex-data.tar.gz — nothing in our build needs to be on the
   non-exec side. nls/ and fonts/ get a separate small tarball if we
   end up needing them.)

The output libwine_*.so names from this script never collide with the
master branch's libwine_*.so names because we install into a separate
filename namespace (4-digit hex starting from f000) and ship our own
manifest. App code switches manifests by branch via WineSetup vs
WineSetupFex.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path
from typing import Iterator


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
    # Wine 10.10 runs fine without the preloader; we lose some memory-layout
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

    # X11 client libs (Termux-built, Bionic-compatible). winex11.drv links
    # against libX11/libXext at build time; Bionic's dynamic linker
    # resolves them from nativeLibraryDir by SONAME. We ship the .so files
    # directly (no libwine_NNNN.so rename) so the SONAMEs match what wine
    # expects. NDK strip below leaves them executable.
    repo_root = build_root.parent.parent.parent
    x11_lib_dir = repo_root / "toolchain/x11-libs"
    for so_name in [
        "libX11.so", "libXau.so", "libxcb.so", "libXdmcp.so",
        "libXext.so", "libXrender.so", "libXi.so", "libXfixes.so",
        "libXrandr.so", "libXcursor.so", "libXxf86vm.so",
        "libandroid-support.so",
        # FreeType chain — wine's gdi32/win32u link against libfreetype.so
        # which pulls libpng16, libbz2 and libbrotlidec (which itself pulls
        # libbrotlicommon).
        # libfreetype + libpng — both built from upstream source against
        # the NDK (see scripts/build-android-libs.sh). They link against
        # Bionic's libz/libm/libdl directly so no further freetype
        # deps need to ride along. Termux's pre-built versions tripped
        # Bionic's linker every which way (symbol versioning, namespace
        # isolation); a clean NDK build sidesteps that entirely.
        "libfreetype.so", "libpng16.so",
    ]:
        src = x11_lib_dir / so_name
        if src.exists():
            yield src, f"_X11_RAW_/{so_name}"

    # GnuTLS for wine's secur32 (Schannel TLS). Built by
    # scripts/build-gnutls-android.sh into toolchain/gnutls-android-arm64/lib.
    # Ships under its original SONAME so wine's runtime link picks it up.
    gnutls_lib = repo_root / "toolchain/gnutls-android-arm64/lib/libgnutls.so"
    if gnutls_lib.exists():
        yield gnutls_lib, "_X11_RAW_/libgnutls.so"


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


def normalize_sonames(path: Path) -> None:
    """patchelf the file in place: strip version suffixes from SONAME and
    NEEDED entries (libz.so.1 → libz.so, libbz2.so.1.0 → libbz2.so).
    Bionic's dynamic linker rejects versioned SONAMEs from APKs.
    No-op if patchelf isn't available or the file isn't ELF."""
    if not shutil.which("patchelf"):
        return
    # readelf to enumerate NEEDED + SONAME
    try:
        out = subprocess.run(
            ["readelf", "-d", str(path)],
            check=True, capture_output=True, text=True,
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return

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
                subprocess.run(
                    ["patchelf", "--replace-needed", orig, normalized, str(path)],
                    check=False, capture_output=True,
                )
        elif "(SONAME)" in line and "[" in line:
            orig = line.split("[", 1)[1].rstrip("]")
            normalized = strip_ver(orig)
            if normalized != orig:
                subprocess.run(
                    ["patchelf", "--set-soname", normalized, str(path)],
                    check=False, capture_output=True,
                )


def strip_into(src: Path, dst: Path, strip_tool: str) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    # Strip is best-effort: PE DLLs may complain, in which case we keep them as-is.
    try:
        subprocess.run([strip_tool, "--strip-unneeded", str(dst)], check=True, capture_output=True)
    except subprocess.CalledProcessError:
        # Fall back to keeping the unstripped copy.
        pass


def main() -> int:
    # Default NDK strip path: $ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
    # If $ANDROID_NDK isn't set, fall back to NDK r26.1 under $HOME/Android/Sdk/ — same
    # default the wine + FEX build scripts use. Override with --strip <path>.
    default_ndk = os.environ.get("ANDROID_NDK") or os.path.expanduser("~/Android/Sdk/ndk/26.1.10909125")
    default_strip = f"{default_ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

    ap = argparse.ArgumentParser()
    ap.add_argument("--repo-root", required=True, type=Path)
    ap.add_argument("--strip", default=default_strip,
                    help="llvm-strip path (defaults to $ANDROID_NDK/.../llvm-strip)")
    args = ap.parse_args()

    repo = args.repo_root.resolve()
    wine_build = repo / "external/wine-upstream/build-android-arm64"
    fex_arm64ec = repo / "external/fex-upstream/build-arm64ec/Bin/libarm64ecfex.dll"
    fex_wow64 = repo / "external/fex-upstream/build-wow64/Bin/libwow64fex.dll"
    out_jni = repo / "src/main/jniLibs/arm64-v8a"
    out_manifest = repo / "src/main/assets/wine-fex-manifest.json"

    for p in [wine_build / "loader/wine", fex_arm64ec, fex_wow64]:
        if not p.exists():
            print(f"missing input: {p}", file=sys.stderr)
            return 1

    out_jni.mkdir(parents=True, exist_ok=True)

    # Wipe any leftover fxxx files from a prior pack to keep the lib namespace clean.
    for stale in out_jni.glob("libwine_f???.so"):
        stale.unlink()

    entries: list[dict] = []
    next_idx = 0xf000  # f-prefix marks "fex-pivot" packaging vs master branch's lower numbers

    # Also wipe any prior X11/gnutls/freetype/png libs we shipped directly
    # under their original names so a re-pack stays clean.
    for stale_name in ("libX11.so", "libXau.so", "libxcb.so", "libXdmcp.so",
                       "libXext.so", "libXrender.so", "libXi.so", "libXfixes.so",
                       "libXrandr.so", "libXcursor.so", "libXxf86vm.so",
                       "libandroid-support.so",
                       "libfreetype.so", "libpng16.so",
                       "libgnutls.so"):
        stale = out_jni / stale_name
        if stale.exists():
            stale.unlink()

    total_bytes = 0
    for src, device_path in iter_build_outputs(wine_build, fex_arm64ec, fex_wow64):
        if not src.exists():
            print(f"  skip (missing): {src}", file=sys.stderr)
            continue
        if not file_needs_exec(src):
            # Just-in-case fallback; everything we list should be ELF or PE.
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

        lib_name = f"libwine_{next_idx:04x}.so"
        next_idx += 1
        dst = out_jni / lib_name
        strip_into(src, dst, args.strip)
        size = dst.stat().st_size
        total_bytes += size
        entries.append({"lib": lib_name, "path": device_path, "size": size})

    with out_manifest.open("w") as f:
        json.dump({
            "schema": 1,
            "wine_root_device": WINE_ROOT_DEVICE,
            "entries": entries,
        }, f, indent=2)

    print(f"packed {len(entries)} libs ({total_bytes/1024/1024:.1f} MB total) → {out_jni}")
    print(f"manifest → {out_manifest}")

    # --- wine NLS tarball -----------------------------------------------------
    # WineSetup.kt extracts this into <wineRoot>/share/wine at runtime.
    # The .nls files are wine's codepage conversion tables; without them
    # plugins doing MultiByteToWideChar (most do) crash on first use.
    wine_src_nls = repo / "external/wine-upstream/nls"
    out_nls_tar = repo / "src/main/assets/wine-fex-nls.tar.gz"
    if wine_src_nls.exists():
        with tarfile.open(out_nls_tar, "w:gz") as tf:
            tf.add(wine_src_nls, arcname="nls", filter=lambda ti:
                ti if ti.name.endswith(".nls") or ti.isdir() else None)
        print(f"NLS tarball → {out_nls_tar} ({out_nls_tar.stat().st_size/1024:.0f} KB)")
    else:
        print(f"WARN: wine NLS source dir not found at {wine_src_nls}", file=sys.stderr)

    # --- wine fonts ----------------------------------------------------------
    # fetch-x11-libs.sh collects Liberation + DejaVu TTFs into toolchain/
    # wine-fonts/. Stage them into assets/wine-fonts/ for WineSetup.kt's
    # seedFonts() to copy into each wineprefix's drive_c/windows/Fonts.
    src_fonts = repo / "toolchain/wine-fonts"
    out_fonts = repo / "src/main/assets/wine-fonts"
    if src_fonts.exists():
        out_fonts.mkdir(parents=True, exist_ok=True)
        # Wipe stale fonts so renames upstream don't leave orphans.
        for stale in out_fonts.glob("*.ttf"):
            stale.unlink()
        count = 0
        for ttf in src_fonts.glob("*.ttf"):
            shutil.copy2(ttf, out_fonts / ttf.name)
            count += 1
        print(f"fonts → {out_fonts} ({count} files)")
    else:
        print(f"WARN: toolchain/wine-fonts not found; run fetch-x11-libs.sh first", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
