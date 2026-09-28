"""
Pruning of the wine PE runtime for pack-wine-fex.py (see ../wine-prune.conf).

plan() decides which PE files to leave out and refuses (PruneError) when that
would break a kept module: every kept PE file's imports, delay-imports and
export forwarders are read with llvm-readobj — including the ARM64EC view
("HybridObject") of wine's ARM64X DLLs — and none may name a pruned file of
the same architecture. It also refuses when a pruned file matches
[dlls.keep] (modules loaded by name at runtime, which that check can't see).
"""

import fnmatch
import hashlib
import re
import shutil
import subprocess
from pathlib import Path

# api-ms-*/ext-ms-* are API-set contracts resolved by wine's loader, not files.
_VIRTUAL = re.compile(r"^(api|ext)-ms-", re.IGNORECASE)


class PruneError(Exception):
    pass


class Config:
    def __init__(self, path: Path):
        self.sha256 = hashlib.sha256(path.read_bytes()).hexdigest()
        sections: dict[str, list[str]] = {}
        current = None
        for raw in path.read_text().splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                current = sections.setdefault(line[1:-1].strip(), [])
            elif current is None:
                raise PruneError(f"{path}: pattern outside a section: {raw!r}")
            else:
                current.append(line.lower())
        unknown = set(sections) - {"programs.keep", "dlls.keep", "dlls.deny"}
        if unknown:
            raise PruneError(f"{path}: unknown section(s) {sorted(unknown)}")
        self.programs_keep = sections.get("programs.keep", [])
        self.dlls_keep = sections.get("dlls.keep", [])
        self.dlls_deny = sections.get("dlls.deny", [])

    @staticmethod
    def _match(name: str, patterns: list[str]) -> bool:
        return any(fnmatch.fnmatchcase(name, p) for p in patterns)

    def is_pruned(self, name: str) -> bool:
        name = name.lower()
        if name.endswith(".exe"):
            return not self._match(name, self.programs_keep)
        return self._match(name, self.dlls_deny)

    def must_keep(self, name: str) -> bool:
        return self._match(name.lower(), self.dlls_keep)


def find_readobj(repo: Path) -> str:
    """The llvm-mingw toolchain's llvm-readobj (a wine-build prerequisite),
    else one from PATH. Needs LLVM >= 19 for the ARM64X hybrid view."""
    bundled = repo / "external/llvm-mingw/install/bin/llvm-readobj"
    if bundled.exists():
        return str(bundled)
    found = shutil.which("llvm-readobj")
    if not found:
        raise PruneError("llvm-readobj not found (build llvm-mingw or install LLVM >= 19)")
    return found


def _dll_name(ref: str) -> str:
    ref = ref.strip().lower()
    return ref if "." in ref else ref + ".dll"


def _parse_readobj(out: str, by_str: dict[str, Path], deps: dict[Path, set[str]]) -> None:
    current, prev = None, ""
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("File: "):
            current = by_str.get(s[len("File: "):])
            if current is None:
                raise PruneError(f"unexpected llvm-readobj output for {s}")
            deps.setdefault(current, set())
        elif current is not None:
            if s.startswith("Name: ") and prev in ("Import {", "DelayImport {"):
                deps[current].add(_dll_name(s[len("Name: "):]))
            elif s.startswith("ForwardedTo: "):
                deps[current].add(_dll_name(s[len("ForwardedTo: "):].rsplit(".", 1)[0]))
        if s:
            prev = s


def read_dependencies(readobj: str, files: list[Path], batch: int = 200) -> dict[Path, set[str]]:
    """Map each PE file to the lowercase DLL names it imports, delay-imports or
    forwards exports to (both views of ARM64X images)."""
    deps: dict[Path, set[str]] = {}
    by_str = {str(f): f for f in files}
    chunks = [[str(f) for f in files[i:i + batch]] for i in range(0, len(files), batch)]
    for chunk in chunks:
        r = subprocess.run([readobj, "--coff-imports", *chunk], capture_output=True, text=True)
        if r.returncode != 0:
            raise PruneError(f"llvm-readobj --coff-imports failed: {r.stderr.strip()[:500]}")
        _parse_readobj(r.stdout, by_str, deps)
    # llvm-readobj rejects an empty export table (a few wine stub DLLs have one)
    # and then stops the whole invocation, so retry a failed batch per file;
    # a file that still fails simply has no exports, hence no forwarders.
    for chunk in chunks:
        r = subprocess.run([readobj, "--coff-exports", *chunk], capture_output=True, text=True)
        if r.returncode == 0:
            _parse_readobj(r.stdout, by_str, deps)
            continue
        for f in chunk:
            r = subprocess.run([readobj, "--coff-exports", f], capture_output=True, text=True)
            if r.returncode == 0:
                _parse_readobj(r.stdout, by_str, deps)
    missing = [f for f in files if f not in deps]
    if missing:
        raise PruneError(f"llvm-readobj produced no output for {missing[:3]}…")
    return deps


def plan(pe_files: list[tuple[Path, str]], config_path: Path, readobj: str) -> dict:
    """pe_files: (source path, device path) of every PE file the pack would
    ship. Returns {"pruned": set(device paths), "report": dict}; raises
    PruneError if pruning would break a kept module."""
    cfg = Config(config_path)
    arch_of = lambda device_path: Path(device_path).parent.name  # aarch64-windows / i386-windows
    pruned = {dp for _, dp in pe_files if cfg.is_pruned(Path(dp).name)}

    problems = [f"{dp}: pruned but matches [dlls.keep]"
                for dp in sorted(pruned) if cfg.must_keep(Path(dp).name)]

    pruned_names = {(arch_of(dp), Path(dp).name.lower()) for dp in pruned}
    kept = [(src, dp) for src, dp in pe_files if dp not in pruned]
    deps = read_dependencies(readobj, [src for src, _ in kept])
    for src, dp in kept:
        for dep in sorted(deps[src]):
            if not _VIRTUAL.match(dep) and (arch_of(dp), dep) in pruned_names:
                problems.append(f"{dp}: kept, but depends on pruned {arch_of(dp)}/{dep}")
    if problems:
        raise PruneError("pruning would break kept modules:\n  " + "\n  ".join(problems))

    unused_deny = [p for p in cfg.dlls_deny
                   if not any(fnmatch.fnmatchcase(Path(dp).name.lower(), p) for dp in pruned)]
    return {
        "pruned": pruned,
        "report": {
            "config_sha256": cfg.sha256,
            "pe_total": len(pe_files),
            "pe_pruned": len(pruned),
            "pe_kept": len(kept),
            "deny_patterns_matching_nothing": unused_deny,
            "pruned": sorted(pruned),
        },
    }
