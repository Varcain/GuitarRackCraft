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

  vt APK|SHA256 [--upload] [--wait] [--children] [--json OUT] [--rpm N]
      VirusTotal lookup (API v3; key from $VT_API_KEY, or from the file named
      by $VT_API_KEY_FILE so it stays out of shell history). --upload submits the
      APK when VirusTotal hasn't seen it (large files via upload_url), --wait
      polls until that analysis completes. Prints the detection stats and
      each engine's verdict; --children lists flagged files inside the APK,
      matched by sha256 against the local APK and shown by real wine path.
      Requests are paced to --rpm (default 4, the public-API quota).

  vt-diff OLD.json NEW.json
      Compare two `vt --json` results: stats, engines, flagged inner files.

Exit status: 0 = ok, 1 = findings that fail the audit, 2 = usage/IO error.
Python stdlib only.
"""

import argparse
import collections
import hashlib
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zipfile
from pathlib import Path

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
            if in_lib:
                real = wine_map.get(os.path.basename(name))
            elif name.startswith("assets/wine/"):
                real = "/" + name[len("assets/"):]  # manifest v2: real names under assets/wine
            else:
                real = None
            location = os.path.dirname(real) if in_lib and real else os.path.dirname(name)
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


# --------------------------------------------------------------------------
# VirusTotal (API v3)

VT_API = "https://www.virustotal.com/api/v3"
VT_DIRECT_UPLOAD_MAX = 32 * 1024 * 1024  # larger files go through /files/upload_url


class VTError(Exception):
    pass


class VT:
    """Minimal VirusTotal v3 client, paced to `rpm` requests per minute."""

    def __init__(self, api_key: str, rpm: float):
        self.api_key = api_key
        self.min_gap = 60.0 / rpm
        self.last = 0.0

    def _request(self, method: str, url: str, body=None, headers: dict | None = None):
        """JSON response, or None on 404. `body` is a zero-arg callable
        returning the request data, so a rate-limited request can be resent."""
        url = url if url.startswith("http") else VT_API + url
        for _ in range(10):
            wait = self.last + self.min_gap - time.time()
            if wait > 0:
                time.sleep(wait)
            self.last = time.time()
            req = urllib.request.Request(url, data=body() if body else None, method=method,
                                         headers={"x-apikey": self.api_key, **(headers or {})})
            try:
                with urllib.request.urlopen(req, timeout=900) as r:
                    return json.load(r)
            except urllib.error.HTTPError as e:
                if e.code == 404:
                    return None
                if e.code == 429:  # quota exceeded: back off and retry
                    time.sleep(60)
                    continue
                raise VTError(f"{method} {url}: HTTP {e.code}: {e.read()[:300].decode(errors='replace')}")
        raise VTError(f"{method} {url}: still rate-limited after retries")

    def file(self, sha256: str) -> dict | None:
        r = self._request("GET", f"/files/{sha256}")
        return r["data"]["attributes"] if r else None

    def upload(self, path: Path) -> str:
        """Upload a file (streamed from disk); returns the analysis id."""
        size = path.stat().st_size
        url = self._request("GET", "/files/upload_url")["data"] if size > VT_DIRECT_UPLOAD_MAX else "/files"
        boundary = uuid.uuid4().hex
        head = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; '
                f'filename="{path.name}"\r\nContent-Type: application/octet-stream\r\n\r\n').encode()
        tail = f"\r\n--{boundary}--\r\n".encode()

        def body():
            yield head
            with path.open("rb") as f:
                while chunk := f.read(1 << 20):
                    yield chunk
            yield tail
        headers = {"Content-Type": f"multipart/form-data; boundary={boundary}",
                   "Content-Length": str(len(head) + size + len(tail))}
        return self._request("POST", url, body=body, headers=headers)["data"]["id"]

    def wait_for_analysis(self, analysis_id: str, timeout_s: int = 3600) -> None:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            r = self._request("GET", f"/analyses/{analysis_id}")
            status = (r or {}).get("data", {}).get("attributes", {}).get("status")
            print(f"  analysis {status or 'pending'}…", file=sys.stderr)
            if status == "completed":
                return
            time.sleep(30)
        raise VTError(f"analysis {analysis_id} not completed after {timeout_s} s")

    def bundled_files(self, sha256: str) -> list[dict]:
        """Files VirusTotal extracted from the APK (full file objects)."""
        out, cursor = [], None
        while True:
            q = "?limit=40" + (f"&cursor={urllib.parse.quote(cursor)}" if cursor else "")
            r = self._request("GET", f"/files/{sha256}/bundled_files{q}")
            if not r:
                return out
            out += r.get("data", [])
            cursor = r.get("meta", {}).get("cursor")
            if not cursor:
                return out


def _verdicts(attrs: dict) -> dict:
    return {engine: res.get("result") for engine, res in attrs.get("last_analysis_results", {}).items()
            if res.get("category") in ("malicious", "suspicious")}


def _apk_index(apk: Path) -> dict[str, str]:
    """sha256 -> display path for every APK entry (real wine path when known)."""
    with zipfile.ZipFile(apk) as zf:
        wine_map = load_wine_manifest(zf)
        index = {}
        for info in zf.infolist():
            if info.is_dir():
                continue
            name = info.filename
            real = wine_map.get(os.path.basename(name)) if name.startswith("lib/") else (
                "/" + name[len("assets/"):] if name.startswith("assets/wine/") else None)
            index[hashlib.sha256(zf.read(info)).hexdigest()] = f"{real}  ({name})" if real else name
    return index


def vt_command(args) -> int:
    api_key = os.environ.get("VT_API_KEY")
    if not api_key and os.environ.get("VT_API_KEY_FILE"):
        api_key = Path(os.environ["VT_API_KEY_FILE"]).expanduser().read_text().strip()
    if not api_key:
        print("error: set VT_API_KEY or VT_API_KEY_FILE (a free VirusTotal community key works)",
              file=sys.stderr)
        return 2
    vt = VT(api_key, args.rpm)
    target = Path(args.target)
    is_file = target.is_file()
    sha = hashlib.sha256(target.read_bytes()).hexdigest() if is_file else args.target.lower()
    print(f"sha256 {sha}")

    attrs = vt.file(sha)
    if attrs is None:
        if not (args.upload and is_file):
            print("not known to VirusTotal (use --upload with a file to submit it)")
            return 1
        if target.stat().st_size > VT_UPLOAD_LIMIT:
            print(f"error: {target.name} exceeds VirusTotal's upload limit", file=sys.stderr)
            return 2
        analysis = vt.upload(target)
        print(f"uploaded, analysis {analysis}")
        if not args.wait:
            return 0
        vt.wait_for_analysis(analysis)
        attrs = vt.file(sha)
        if attrs is None:
            raise VTError("analysis completed but the file report is missing")

    stats = attrs.get("last_analysis_stats", {})
    verdicts = _verdicts(attrs)
    print(f"stats: {stats}")
    for engine, result in sorted(verdicts.items()):
        print(f"  {engine:24s} {result}")

    children = []
    if args.children:
        try:
            objs = vt.bundled_files(sha)
        except VTError as e:
            print(f"bundled files unavailable ({e}); skipping", file=sys.stderr)
            objs = []
        index = _apk_index(target) if is_file else {}
        for obj in objs:
            a = obj.get("attributes", {})
            v = _verdicts(a)
            if not v:
                continue
            path = index.get(obj.get("id"), (a.get("names") or [obj.get("id")])[0])
            children.append({"sha256": obj.get("id"), "path": path, "engines": len(v), "verdicts": v})
        children.sort(key=lambda c: -c["engines"])
        print(f"flagged files inside ({len(children)} of {len(objs)} VirusTotal extracted):")
        for c in children:
            print(f"  {c['engines']:3d}  {c['path']}  e.g. {next(iter(c['verdicts'].values()))}")

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"sha256": sha, "stats": stats, "verdicts": verdicts, "children": children}, f, indent=1)
    return 0


def vt_diff_command(args) -> int:
    old, new = (json.load(open(p)) for p in (args.old, args.new))
    mal = lambda r: r["stats"].get("malicious", 0) + r["stats"].get("suspicious", 0)
    print(f"flagged by: {mal(old)} -> {mal(new)} engines")
    gone = sorted(set(old["verdicts"]) - set(new["verdicts"]))
    added = sorted(set(new["verdicts"]) - set(old["verdicts"]))
    print(f"  engines no longer flagging: {', '.join(gone) or '-'}")
    print(f"  engines newly flagging:     {', '.join(added) or '-'}")
    key = lambda c: c["path"].split("  (")[0]
    o = {key(c): c for c in old.get("children", [])}
    n = {key(c): c for c in new.get("children", [])}
    print(f"flagged inner files: {len(o)} -> {len(n)}")
    for p in sorted(set(o) - set(n)):
        print(f"  - {p} ({o[p]['engines']})")
    for p in sorted(set(n) - set(o)):
        print(f"  + {p} ({n[p]['engines']})")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("audit", help="offline PE inventory / masquerading check of an APK")
    a.add_argument("apk")
    a.add_argument("--json", metavar="OUT", help="write the full audit result as JSON")
    a.add_argument("--baseline", metavar="OLD.json", help="diff against a previous --json result")
    a.add_argument("--allow-pe-in-lib", action="store_true",
                   help="report PE files under lib/ as a warning instead of failing")
    v = sub.add_parser("vt", help="VirusTotal lookup / upload (needs $VT_API_KEY)")
    v.add_argument("target", help="APK path or sha256")
    v.add_argument("--upload", action="store_true", help="submit the file if VirusTotal doesn't know it")
    v.add_argument("--wait", action="store_true", help="after --upload, wait for the analysis")
    v.add_argument("--children", action="store_true", help="list flagged files inside the APK")
    v.add_argument("--json", metavar="OUT", help="write the result for vt-diff")
    v.add_argument("--rpm", type=float, default=4, help="requests per minute (public API: 4)")
    d = sub.add_parser("vt-diff", help="compare two `vt --json` results")
    d.add_argument("old")
    d.add_argument("new")
    args = ap.parse_args()

    if args.cmd == "vt":
        try:
            return vt_command(args)
        except (VTError, OSError, urllib.error.URLError) as e:
            print(f"error: {e}", file=sys.stderr)
            return 2
    if args.cmd == "vt-diff":
        return vt_diff_command(args)

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
