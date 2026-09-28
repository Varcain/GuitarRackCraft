#!/usr/bin/env python3
"""
tools/vt_check.py — antivirus-sensitivity checks for built APKs.

The full (Windows-VST) APK was flagged by ~15 VirusTotal engines, and every
flagged file inside it was a Windows PE binary (Wine builtins), shipped
disguised as lib/arm64-v8a/libwine_*.so. This tool measures exactly that, so
packaging changes can be checked offline, with no API key.

  audit APK [--json OUT] [--baseline OLD.json] [--allow-pe-in-lib]
      Inventory every Windows PE file in the APK (grouped by location /
      architecture / DLL-vs-EXE; libwine_*.so names are mapped back to their
      real Wine paths via assets/wine-fex-manifest.json), flag PE files hidden
      under lib/ (error unless --allow-pe-in-lib) and other non-ELF files
      there, and warn when the APK exceeds VirusTotal's upload limit.
      --json writes a machine-readable summary; --baseline diffs against a
      previous --json output.

Exit status: 0 = ok, 1 = findings that fail the audit, 2 = usage/IO error.
Python stdlib only.
"""

import argparse
import collections
import json
import os
import struct
import sys
import zipfile

VT_UPLOAD_LIMIT = 650 * 1024 * 1024  # VirusTotal's max upload size (via upload_url)
WINE_MANIFEST = "assets/wine-fex-manifest.json"

PE_MACHINES = {0x014C: "i386", 0x8664: "x64", 0xAA64: "arm64", 0xA641: "arm64ec", 0xA64E: "arm64x"}
IMAGE_FILE_DLL = 0x2000


def pe_info(head: bytes):
    """(arch, kind) for a PE image header, or None if `head` isn't a PE file."""
    if len(head) < 0x40 or head[:2] != b"MZ":
        return None
    e_lfanew = struct.unpack_from("<I", head, 0x3C)[0]
    if e_lfanew + 24 > len(head) or head[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        return None
    machine, = struct.unpack_from("<H", head, e_lfanew + 4)
    characteristics, = struct.unpack_from("<H", head, e_lfanew + 22)
    arch = PE_MACHINES.get(machine, f"0x{machine:04x}")
    return arch, ("dll" if characteristics & IMAGE_FILE_DLL else "exe")


def read_head(zf: zipfile.ZipFile, info: zipfile.ZipInfo, n: int = 4096) -> bytes:
    with zf.open(info) as f:
        return f.read(n)


def load_wine_manifest(zf: zipfile.ZipFile) -> dict:
    """lib name -> real device path, from the packer's manifest (if present)."""
    try:
        data = json.loads(zf.read(WINE_MANIFEST))
    except KeyError:
        return {}
    return {e["lib"]: e["path"] for e in data.get("entries", []) if "lib" in e}


def audit(apk_path: str) -> dict:
    with zipfile.ZipFile(apk_path) as zf:
        wine_map = load_wine_manifest(zf)
        pe_files, non_elf_in_lib = [], []
        lib_entries = 0
        for info in zf.infolist():
            if info.is_dir() or info.file_size < 64:
                continue
            name = info.filename
            in_lib = name.startswith("lib/")
            lib_entries += in_lib
            head = read_head(zf, info)
            pe = pe_info(head)
            if pe is None:
                if in_lib and head[:4] != b"\x7fELF":
                    non_elf_in_lib.append(name)
                continue
            arch, kind = pe
            real = wine_map.get(os.path.basename(name)) if in_lib else None
            location = os.path.dirname(real) if real else os.path.dirname(name)
            pe_files.append({
                "entry": name, "real_path": real, "arch": arch, "kind": kind,
                "size": info.file_size, "in_lib": in_lib,
                "group": f"{'lib/ as .so -> ' if in_lib else ''}{location or '/'}",
            })

    groups = collections.Counter((p["group"], p["arch"], p["kind"]) for p in pe_files)
    return {
        "apk": os.path.basename(apk_path),
        "apk_size": os.path.getsize(apk_path),
        "lib_entries": lib_entries,
        "pe_total": len(pe_files),
        "pe_in_lib": sum(p["in_lib"] for p in pe_files),
        "non_elf_in_lib": non_elf_in_lib,
        "pe_groups": [{"group": g, "arch": a, "kind": k, "count": c}
                      for (g, a, k), c in sorted(groups.items())],
        "pe_files": sorted(pe_files, key=lambda p: p["entry"]),
    }


def pe_key(p: dict) -> str:
    """Stable identity for a PE file across builds: its real path when known."""
    return p["real_path"] or p["entry"]


def print_report(r: dict, baseline: dict | None, allow_pe_in_lib: bool) -> int:
    mb = lambda n: f"{n / 2**20:.1f} MiB"
    print(f"APK: {r['apk']} ({mb(r['apk_size'])})")
    print(f"Windows PE files: {r['pe_total']}  (hidden under lib/ as .so: {r['pe_in_lib']})")
    width = max((len(g["group"]) for g in r["pe_groups"]), default=0)
    for g in r["pe_groups"]:
        print(f"  {g['group']:<{width}}  {g['arch']:<7} {g['kind']:<3} {g['count']:>5}")

    failed = False
    if r["pe_in_lib"]:
        level = "WARN" if allow_pe_in_lib else "ERROR"
        failed |= not allow_pe_in_lib
        print(f"{level}: {r['pe_in_lib']} Windows PE files are packaged under lib/ with a .so name "
              "(file-type masquerading; scanners flag this)")
    if r["non_elf_in_lib"]:
        print(f"WARN: {len(r['non_elf_in_lib'])} non-ELF files under lib/, e.g. {r['non_elf_in_lib'][0]}")
    if r["apk_size"] > VT_UPLOAD_LIMIT:
        print(f"WARN: APK exceeds VirusTotal's {mb(VT_UPLOAD_LIMIT)} upload limit")

    if baseline:
        print(f"Baseline: {baseline['apk']}")
        for key in ("apk_size", "pe_total", "pe_in_lib"):
            old, new = baseline[key], r[key]
            fmt = mb if key == "apk_size" else str
            print(f"  {key:<9} {fmt(old):>10} -> {fmt(new):>10}")
        old_set = {pe_key(p) for p in baseline["pe_files"]}
        new_set = {pe_key(p) for p in r["pe_files"]}
        print(f"  PE files removed: {len(old_set - new_set)}, added: {len(new_set - old_set)}")
        for name in sorted(new_set - old_set)[:20]:
            print(f"    + {name}")

    print("FAIL" if failed else "OK")
    return 1 if failed else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("audit", help="offline PE inventory / masquerading check of an APK")
    a.add_argument("apk")
    a.add_argument("--json", metavar="OUT", help="write the full audit result as JSON")
    a.add_argument("--baseline", metavar="OLD.json", help="diff against a previous --json result")
    a.add_argument("--allow-pe-in-lib", action="store_true",
                   help="report PE files under lib/ as a warning instead of failing")
    args = ap.parse_args()

    try:
        result = audit(args.apk)
        baseline = json.load(open(args.baseline)) if args.baseline else None
    except (OSError, zipfile.BadZipFile, json.JSONDecodeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if args.json:
        with open(args.json, "w") as f:
            json.dump(result, f, indent=1)
    return print_report(result, baseline, args.allow_pe_in_lib)


if __name__ == "__main__":
    sys.exit(main())
