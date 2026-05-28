#!/usr/bin/env python3
"""
Triage a vst_host_<uuid>.log into a 1-page health report.

Streaming parser (line-by-line) so a 500 MB log finishes in <5 s without
loading the whole file. Recognises ~20 known signatures for plugin
lifecycle, DXVK/D3D11 init, VEH events, exceptions, JUCE event-loop
patterns, and X11 paint activity. Cross-references the agent memory
index at /home/varcain/.claude/projects/-home-varcain-projects-private-vstpoc/memory/MEMORY.md
to surface relevant [[memory-name]] links in the diagnosis line.

Usage:
  triage-vst-log.py <local-log-path>
  triage-vst-log.py --adb <uuid>           # auto-pull via run-as
  triage-vst-log.py --full <path>          # dump every counter
  triage-vst-log.py --since 22:00 <path>   # filter to events after timestamp

Designed for an agent (claude / human) to drop one command after a
failing plugin and get an actionable summary, instead of grep-ing a
multi-hundred-MB log.
"""

from __future__ import annotations

import argparse
import collections
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

MEMORY_INDEX = Path(
    "/home/varcain/.claude/projects/-home-varcain-projects-private-vstpoc/memory/MEMORY.md"
)
APP_PACKAGE = "com.varcain.guitarrackcraft"
CACHE_LOG_PREFIX = "cache/vst_host_v"


# ---------------------------------------------------------------------------
# State machine
# ---------------------------------------------------------------------------

@dataclass
class Counter:
    count: int = 0
    first_ts: str = ""
    last_ts: str = ""
    extra: list[str] = field(default_factory=list)

    def hit(self, ts: str, extra: str | None = None) -> None:
        if self.count == 0:
            self.first_ts = ts
        self.last_ts = ts
        self.count += 1
        if extra and len(self.extra) < 4:
            self.extra.append(extra)


@dataclass
class TriageState:
    log_size: int = 0
    log_path: str = ""

    # lifecycle
    init_peb: Counter = field(default_factory=Counter)
    guest_ready_after_ms: int | None = None
    init_peb_ts: str = ""

    # loaded DLLs by basename (lowercase, no path)
    dlls_loaded: set[str] = field(default_factory=set)

    # DXVK / D3D
    dxvk_version: str = ""
    dxvk_adapter: str = ""
    d3d11_max_feature_level: str = ""
    d3d11_selected_feature_level: str = ""
    d3d11_create_device_failed: Counter = field(default_factory=Counter)
    dxvk_memory_alloc_failed: Counter = field(default_factory=Counter)
    dxvk_memory_alloc_size: int = 0
    dxvk_memory_alloc_types: str = ""
    dxvk_bisect_culprit: str = ""

    # VEH
    veh_thu_popup_dismiss: Counter = field(default_factory=Counter)
    veh_thu_effect_removal: Counter = field(default_factory=Counter)
    veh_tonex_null_ctrl: Counter = field(default_factory=Counter)
    veh_write_null: Counter = field(default_factory=Counter)
    veh_general_null: Counter = field(default_factory=Counter)
    veh_write_null_bytes: str = ""

    # exceptions
    access_violations: Counter = field(default_factory=Counter)

    # JUCE / paint
    wm_user_123: Counter = field(default_factory=Counter)
    wm_user_123_hwnd: str = ""
    wm_paint: Counter = field(default_factory=Counter)
    x11_putimage: Counter = field(default_factory=Counter)

    # network
    dnsapi_no_resolv: Counter = field(default_factory=Counter)

    # missing-DLL imports — "Library X.dll (which is needed by Y) not found".
    # A high-value signal: a plugin whose rendering DLL chain has a broken
    # link (e.g. AmpliTube's d2d1.dll → missing d3d10_1.dll) renders black
    # even though nothing crashes. Keyed by the missing dll name.
    missing_imports: dict[str, str] = field(default_factory=dict)

    # generic err/warn buckets
    err_lines: int = 0
    warn_lines: int = 0
    fixme_lines: int = 0

    # raw text bookkeeping for last-N-err lines (avoids the whole-file
    # re-grep an agent would otherwise need)
    recent_err: collections.deque[tuple[str, str]] = field(
        default_factory=lambda: collections.deque(maxlen=8)
    )


# Anchored at left because wine logs start with "TS:" or "tid:" prefixes
# without a leading space.
RE_TS = re.compile(r"^([0-9a-f]{4}):")  # wine tid prefix; mostly skipped

# Wine's log lines don't carry wall-clock by default; we capture the line
# index instead and only the first/last "ts" is "line=N" for ordering.
# When run from --adb mode we do have file mtime to anchor.


_dll_load_re = re.compile(r'load_dll\(L"([^"]+)"\)')
_dll_done_re = re.compile(r'load_dll DONE L"([^"]+)"\s*->\s*(0x[0-9a-fA-F]+)')


def basename_lc(s: str) -> str:
    s = s.replace("\\\\", "\\").replace("\\", "/").strip()
    return os.path.basename(s).lower()


def parse_line(state: TriageState, line: str, lineno: int) -> None:
    """Single-line signature dispatch. ts is line number since wine
    logs don't carry wall clock by default."""
    ts = f"L{lineno}"

    # --- lifecycle ---
    if "err:environ:init_peb starting" in line:
        state.init_peb.hit(ts)
        if "ARM64EC" in line:
            state.init_peb_ts = "ARM64EC"
        elif "wow64" in line.lower():
            state.init_peb_ts = "wow64 (i386)"
        return

    if "load_dll DONE" in line:
        m = _dll_done_re.search(line)
        if m:
            name = basename_lc(m.group(1))
            if name and "." in name:
                state.dlls_loaded.add(name)
        return

    # --- DXVK / D3D ---
    if "info:  DXVK: v" in line:
        # "info:  DXVK: v2.5.3+"
        try:
            state.dxvk_version = line.split("DXVK:")[1].strip()
        except Exception:
            state.dxvk_version = "yes"
        return

    if "Adreno (" in line or "info:    Driver :" in line:
        if state.dxvk_adapter == "" and "Driver" in line:
            state.dxvk_adapter = line.split("Driver :")[1].strip()
        return

    if "D3D11InternalCreateDevice: Maximum supported feature level" in line:
        state.d3d11_max_feature_level = line.split("feature level:")[1].strip()
        return

    if "D3D11InternalCreateDevice: Using feature level" in line:
        state.d3d11_selected_feature_level = line.split("feature level")[1].strip()
        return

    if "D3D11InternalCreateDevice: Failed to create D3D11 device" in line:
        state.d3d11_create_device_failed.hit(ts)
        return

    if "DxvkMemoryAllocator: Memory allocation failed" in line:
        state.dxvk_memory_alloc_failed.hit(ts)
        return

    if "Mem types:" in line and state.dxvk_memory_alloc_failed.count > 0:
        try:
            state.dxvk_memory_alloc_types = line.split("Mem types:")[1].strip()
        except Exception:
            pass
        return

    if "Size:" in line and state.dxvk_memory_alloc_failed.count > 0 and state.dxvk_memory_alloc_size == 0:
        # wine prepends "err:    " so we can't startswith("Size:") — but
        # "Size:" only appears in DxvkMemoryAllocator's three-line block
        # (Size:, Alignment:, Mem types:) so a contains check is safe.
        try:
            state.dxvk_memory_alloc_size = int(
                line.split("Size:")[1].strip().split()[0]
            )
        except Exception:
            pass
        return

    if "DxvkAdapter: bisect: CULPRIT struct sType=" in line:
        state.dxvk_bisect_culprit = line.split("sType=")[1].strip()
        return

    # --- VEH ---
    if "[vst3_host] VEH:" in line:
        if "surgical skip TH-U +0x2DF0C3" in line:
            state.veh_thu_popup_dismiss.hit(ts)
            return
        if "surgical skip TH-U +0x2A05AF" in line:
            state.veh_thu_effect_removal.hit(ts)
            return
        if "surgical skip TONEX-pattern NULL controller" in line:
            state.veh_tonex_null_ctrl.hit(ts)
            return
        if "WRITE-to-NULL AV" in line:
            state.veh_write_null.hit(ts)
            return
        if "NULL-deref AV pc=" in line:
            state.veh_general_null.hit(ts)
            return
        if "bytes@pc:" in line and state.veh_write_null_bytes == "":
            try:
                state.veh_write_null_bytes = line.split("bytes@pc:")[1].strip()
            except Exception:
                pass
            return
        return

    # --- exceptions ---
    if "EXCEPTION_ACCESS_VIOLATION" in line:
        state.access_violations.hit(ts)
        return

    # --- JUCE / paint ---
    if "WM_USER+123" in line or "msg 47b" in line:
        state.wm_user_123.hit(ts)
        if state.wm_user_123_hwnd == "":
            m = re.search(r"hwnd (?:0x|=0x)([0-9a-fA-F]+)", line)
            if m:
                state.wm_user_123_hwnd = "0x" + m.group(1)
        return

    if "WM_PAINT" in line or "msg 000f" in line.lower():
        state.wm_paint.hit(ts)
        return

    if "xrep:" in line and " 4a " in line:
        # X11 op 74 = PutImage; reply opcode 1 carries seq+op; cheap heuristic
        # (we don't fully parse xrep). Better signal: presence of "PutImage"
        # in X11NativeDisplay logs would need logcat, not vst_host.log.
        pass

    if "PutImage" in line:
        state.x11_putimage.hit(ts)
        return

    # --- missing-DLL imports (broken render chain) ---
    if "import_dll Library" in line and "not found" in line:
        # "err:module:import_dll Library d3d10_1.dll (which is needed by
        #  L"C:\\windows\\system32\\d2d1.dll") not found"
        m = re.search(r'import_dll Library (\S+?\.dll)\s*\(which is needed by\s*L?"?([^")]+)', line)
        if m:
            missing = m.group(1)
            needed_by = os.path.basename(m.group(2).replace("\\\\", "/").replace("\\", "/").strip())
            if missing not in state.missing_imports:
                state.missing_imports[missing] = needed_by
        return

    # --- network ---
    if "dnsapi:DllMain No libresolv support" in line:
        state.dnsapi_no_resolv.hit(ts)
        return

    # --- generic err/warn buckets ---
    if "err:" in line:
        state.err_lines += 1
        # capture lines that AREN'T high-frequency noise
        if "err:msg:process_mouse_message" not in line:
            short = line.rstrip()
            if len(short) > 160:
                short = short[:160] + "…"
            state.recent_err.append((ts, short))
        return
    if "warn:" in line:
        state.warn_lines += 1
        return
    if "fixme:" in line:
        state.fixme_lines += 1
        return


# ---------------------------------------------------------------------------
# Diagnosis heuristics
# ---------------------------------------------------------------------------

SYMBOL_MAP = Path(__file__).resolve().parent.parent / "src/main/assets/wine-symbol-map.txt"


def max_module_offset(module: str) -> int:
    """Return the largest symbol offset present for `module` in the symbol
    map, or 0 if the module/map is absent. Used to detect addresses that
    are too large to be a real module offset (= FEX JIT region)."""
    if not SYMBOL_MAP.exists():
        return 0
    biggest = 0
    try:
        with open(SYMBOL_MAP) as f:
            for line in f:
                parts = line.split(None, 2)
                if len(parts) >= 2 and parts[0] == module:
                    try:
                        off = int(parts[1], 16)
                        if off > biggest:
                            biggest = off
                    except ValueError:
                        pass
    except Exception:
        return 0
    return biggest


def annotate_write_null(state: "TriageState") -> str:
    """Explain the WRITE-NULL crash bytes in light of the FEX-JIT reality.

    The VEH dumps `bytes@pc` from the x86_64 *logical* ExceptionAddress.
    For a fault in FEX-translated native ARM64 code that x86 address has
    no static module mapping — the bytes are whatever happens to sit at
    the logical PC, NOT the faulting ARM64 instruction. We detect this by
    noting the historical TONEX downstream crash is at an offset
    (~0xFE7784) far larger than any wine .so (ntdll.so tops out ~0x78050),
    so it cannot be a clean module offset.

    Returns a short annotation string (empty if no WRITE-NULL seen)."""
    if state.veh_write_null.count == 0:
        return ""
    note = (
        "WRITE-NULL bytes are the x86 logical view; if the fault is in "
        "FEX-JIT'd native ARM64 wine code the byte dump + symbol map will "
        "NOT resolve it (JIT regions aren't in any module). "
    )
    # If the captured bytes match the known TONEX-downstream signature,
    # say so directly.
    b = state.veh_write_null_bytes.strip().lower()
    if b.startswith("48 85 00 f8"):
        note += (
            "Bytes start '48 85 00 f8' = the known TONEX setProcessing "
            "downstream STUR-loop site — fix is rax→zero-scratch in the "
            "kTonexPattern VEH handler (see [[tonex-vst3-editor-stall]])."
        )
    return note


def load_memory_index() -> list[tuple[str, str, str]]:
    """Return list of (name, file, hook-text) tuples from MEMORY.md.
    Empty list if index can't be read (no fatal — diagnosis just doesn't
    cross-reference)."""
    if not MEMORY_INDEX.exists():
        return []
    out = []
    for raw in MEMORY_INDEX.read_text(errors="replace").splitlines():
        # Lines look like:  - [Title](file.md) — one-line hook
        m = re.match(r"^- \[([^\]]+)\]\(([^)]+)\)\s*(?:[—-]\s*(.+))?$", raw)
        if m:
            out.append((m.group(1), m.group(2), (m.group(3) or "").strip()))
    return out


def cross_reference(state: TriageState, memory: list[tuple[str, str, str]]) -> list[str]:
    """Match state signals against memory index keywords. Returns
    a list of [[name]] references the diagnosis should surface."""
    if not memory:
        return []
    matches: list[str] = []
    text = " ".join(
        [
            "tonex" if state.veh_tonex_null_ctrl.count else "",
            "thu" if (state.veh_thu_popup_dismiss.count or state.veh_thu_effect_removal.count) else "",
            "amplitube" if ("amplitube" in (state.dxvk_adapter + " ".join(state.dlls_loaded))) else "",
            "memory allocation" if state.dxvk_memory_alloc_failed.count else "",
            "robustness2" if state.dxvk_bisect_culprit.startswith("1000286000") else "",
            "WM_USER+123" if state.wm_user_123.count > 5000 else "",
            "write" if state.veh_write_null.count else "",
            "wineserver" if state.veh_write_null.count else "",
        ]
    ).lower()

    for name, _file, hook in memory:
        hookl = hook.lower()
        name_slug = re.sub(r"[^a-z0-9 ]", " ", name.lower())
        if any(
            keyword and (keyword in hookl or keyword in name_slug)
            for keyword in [
                "tonex" if "tonex" in text else "",
                "thu" if "thu" in text else "",
                "amplitube" if "amplitube" in text else "",
                "memory" if "memory allocation" in text else "",
                "robustness2" if "robustness2" in text else "",
                "juce storm" if "wm_user+123" in text else "",
                "write" if "write" in text else "",
            ]
        ):
            matches.append(f"[[{Path(_file).stem}]]")
    # dedupe preserving order
    seen = set()
    dedup = []
    for m in matches:
        if m not in seen:
            seen.add(m)
            dedup.append(m)
    return dedup[:6]  # cap at 6 to keep diagnosis terse


def derive_diagnosis(state: TriageState, refs: list[str]) -> list[str]:
    """Produce 1-5 lines of human-readable diagnosis from state."""
    lines: list[str] = []

    # No DXVK device + memory failure → AmpliTube-class issue
    if (
        state.dxvk_memory_alloc_failed.count > 0
        and "D3D11InternalCreateDevice" not in (
            state.d3d11_selected_feature_level + state.d3d11_max_feature_level
        )
    ):
        lines.append(
            "DXVK adapter initialised but D3D11 device creation never logged. "
            "DxvkMemoryAllocator failed — Adreno+DXVK 2.5.3 memory wall."
        )
    elif state.dxvk_memory_alloc_failed.count > 0 and state.d3d11_selected_feature_level:
        lines.append(
            f"D3D11 device created at FL{state.d3d11_selected_feature_level.strip()} "
            f"but downstream memory allocation failed "
            f"(size={state.dxvk_memory_alloc_size}, types={state.dxvk_memory_alloc_types})."
        )

    if state.veh_tonex_null_ctrl.count > 0 and state.veh_write_null.count > 0:
        lines.append(
            "TONEX kTonexPattern NULL-controller skipped, then downstream WRITE-NULL."
        )
        wn = annotate_write_null(state)
        if wn:
            lines.append(wn)
    elif state.veh_write_null.count > 0:
        wn = annotate_write_null(state)
        if wn:
            lines.append(wn)

    # Missing render-DLL import is a strong black-screen signal — surface
    # it FIRST since it's directly actionable (symlink the dll in).
    render_dlls = {"d3d10_1.dll", "d3d10.dll", "d3d10core.dll", "d3d11.dll",
                   "d3d9.dll", "dxgi.dll", "d2d1.dll", "dwrite.dll",
                   "dcomp.dll", "d3dcompiler_47.dll"}
    render_missing = {k: v for k, v in state.missing_imports.items()
                      if k.lower() in render_dlls or v.lower() in render_dlls}
    if render_missing:
        for missing, needed_by in render_missing.items():
            lines.append(
                f"MISSING render DLL: {missing} (needed by {needed_by}) — broken "
                f"render chain → black editor even with no crash. Fix: symlink "
                f"the wine builtin into the prefix's system32 (compare a working "
                f"plugin's prefix with snapshot-vst-state.sh)."
            )

    # JUCE storm with little/no painting. wm_paint <= a few is still 'stuck'
    # (AmpliTube emitted 4 WM_PAINT then stalled), so use a low threshold
    # rather than ==0.
    if (state.wm_user_123.count > 5000
            and state.x11_putimage.count == 0
            and state.wm_paint.count < 16):
        lines.append(
            f"JUCE WM_USER+123 storm ({state.wm_user_123.count}) to "
            f"{state.wm_user_123_hwnd or '<hwnd?>'} with only "
            f"{state.wm_paint.count} WM_PAINT and no X11 PutImage — editor "
            f"barely/never repaints (black-screen stall signature)."
        )

    if state.dxvk_bisect_culprit:
        lines.append(
            f"DXVK pNext bisect identified culprit feature struct sType={state.dxvk_bisect_culprit}."
        )

    if state.dnsapi_no_resolv.count > 0 and state.veh_general_null.count > 0:
        lines.append(
            "Plugin attempted phone-home (dnsapi stubbed) before a NULL-deref crash — "
            "possible license-check stall."
        )

    if not lines:
        if state.access_violations.count == 0 and state.dxvk_version:
            lines.append("No exceptions, DXVK loaded — plugin may have rendered normally or be still warming up.")
        else:
            lines.append(
                "No high-confidence diagnosis. Check 'recent err lines' section below."
            )

    if refs:
        lines.append("Cross-refs: " + ", ".join(refs))

    return lines


# ---------------------------------------------------------------------------
# Pretty printer
# ---------------------------------------------------------------------------

def fmt_count(c: Counter, suffix: str = "") -> str:
    if c.count == 0:
        return "0"
    rng = f"{c.first_ts}" + (f"→{c.last_ts}" if c.last_ts != c.first_ts else "")
    return f"{c.count} hits [{rng}]{suffix}"


def report(state: TriageState, full: bool, refs: list[str]) -> str:
    out: list[str] = []
    out.append(f"=== triage: {state.log_path} ({state.log_size//(1024*1024)} MB) ===\n")

    out.append("Lifecycle:")
    out.append(f"  init_peb      {state.init_peb.count} ({state.init_peb_ts or 'unknown mode'})")
    if state.dlls_loaded:
        notable = sorted(
            d
            for d in state.dlls_loaded
            if d
            in {
                "d3d9.dll",
                "d3d10.dll",
                "d3d10core.dll",
                "d3d11.dll",
                "d3d12.dll",
                "dxgi.dll",
                "opengl32.dll",
                "winevulkan.dll",
                "wined3d.dll",
                "gdi32.dll",
            }
            or d.startswith("juce")
        )
        out.append(f"  notable DLLs  {', '.join(notable) or '(none of d3d*/gl/winevulkan)'}")
    out.append("")

    out.append("Graphics:")
    out.append(f"  DXVK          {state.dxvk_version or 'not loaded'}")
    if state.dxvk_adapter:
        out.append(f"  Adapter       {state.dxvk_adapter}")
    if state.d3d11_max_feature_level:
        out.append(
            f"  D3D11 levels  max={state.d3d11_max_feature_level} "
            f"selected={state.d3d11_selected_feature_level.strip() or '(none)'}"
        )
    if state.dxvk_memory_alloc_failed.count > 0:
        out.append(
            f"  Memory alloc  FAILED ({state.dxvk_memory_alloc_failed.count}× "
            f"size={state.dxvk_memory_alloc_size} types={state.dxvk_memory_alloc_types})  X"
        )
    if state.dxvk_bisect_culprit:
        out.append(f"  Bisect        culprit sType={state.dxvk_bisect_culprit}")
    if state.d3d11_create_device_failed.count > 0:
        out.append(f"  D3D11Device   FAILED ({state.d3d11_create_device_failed.count}×)  X")
    out.append(f"  X11 PutImage  {state.x11_putimage.count} ops" + ("  (stalled)" if state.x11_putimage.count == 0 and state.wm_user_123.count > 1000 else ""))
    out.append("")

    out.append("VEH events:")
    any_veh = False
    if state.veh_thu_popup_dismiss.count:
        any_veh = True
        out.append(f"  thu-popup-dismiss          {fmt_count(state.veh_thu_popup_dismiss)}")
    if state.veh_thu_effect_removal.count:
        any_veh = True
        out.append(f"  thu-effect-removal-null    {fmt_count(state.veh_thu_effect_removal)}")
    if state.veh_tonex_null_ctrl.count:
        any_veh = True
        out.append(f"  tonex-null-controller      {fmt_count(state.veh_tonex_null_ctrl)}")
    if state.veh_write_null.count:
        any_veh = True
        out.append(f"  WRITE-to-NULL AV           {fmt_count(state.veh_write_null)} → ExitThread")
        if state.veh_write_null_bytes:
            out.append(f"    bytes@pc: {state.veh_write_null_bytes[:50]}")
    if state.veh_general_null.count:
        any_veh = True
        out.append(f"  general NULL-deref AV      {fmt_count(state.veh_general_null)} → ExitThread")
    if not any_veh:
        out.append("  (none)")
    out.append("")

    out.append("Exceptions:")
    out.append(f"  EXCEPTION_ACCESS_VIOLATION  {state.access_violations.count}")
    out.append("")

    out.append("JUCE / paint:")
    if state.wm_user_123.count > 0:
        marker = "  (RUNAWAY STORM)" if state.wm_user_123.count > 5000 else ""
        out.append(f"  WM_USER+123    {state.wm_user_123.count} → {state.wm_user_123_hwnd or '<hwnd?>'}" + marker)
    out.append(f"  WM_PAINT       {state.wm_paint.count}" + ("  (never painted!)" if state.wm_paint.count == 0 and state.wm_user_123.count > 100 else ""))
    out.append("")

    if state.missing_imports:
        out.append("Missing DLL imports:")
        for missing, needed_by in state.missing_imports.items():
            out.append(f"  {missing}  (needed by {needed_by})")
        out.append("")

    if state.dnsapi_no_resolv.count > 0:
        out.append("Network:")
        out.append(f"  dnsapi: No libresolv support × {state.dnsapi_no_resolv.count}  (phone-home stub)")
        out.append("")

    out.append("Generic counts:")
    out.append(f"  err:   {state.err_lines}    warn: {state.warn_lines}    fixme: {state.fixme_lines}")
    out.append("")

    if state.recent_err:
        out.append("Most recent notable err lines:")
        for ts, line in state.recent_err:
            out.append(f"  [{ts}] {line}")
        out.append("")

    out.append("Diagnosis (heuristic):")
    for diag in derive_diagnosis(state, refs):
        out.append(f"  {diag}")

    if full:
        out.append("")
        out.append("--- All DLLs loaded ---")
        for d in sorted(state.dlls_loaded):
            out.append(f"  {d}")

    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# Input handling
# ---------------------------------------------------------------------------

def stream_local(path: Path) -> Iterable[str]:
    """Stream a local file line-by-line. Tolerant of non-utf8."""
    with open(path, "rb") as f:
        for raw in f:
            yield raw.decode("utf-8", errors="replace")


def stream_adb(uuid: str) -> Iterable[str]:
    """Stream the device's log via adb shell run-as cat. No staging on
    host disk — we pipe stdout straight in."""
    # The log path is files/cache/vst_host_v<uuid>.log (note "cache" is
    # under the app's filesDir, accessed via run-as).
    remote = f"cache/vst_host_v{uuid}.log"
    cmd = ["adb", "shell", "run-as", APP_PACKAGE, "cat", remote]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert proc.stdout is not None
    try:
        for raw in proc.stdout:
            yield raw.decode("utf-8", errors="replace")
    finally:
        proc.terminate()


def adb_log_size(uuid: str) -> int:
    """Cheap size probe so the header reports MB even for adb-mode streams."""
    remote = f"cache/vst_host_v{uuid}.log"
    try:
        out = subprocess.check_output(
            ["adb", "shell", "run-as", APP_PACKAGE, "stat", "-c", "%s", remote],
            stderr=subprocess.DEVNULL,
        )
        return int(out.strip())
    except Exception:
        return 0


def plugin_name_from_registry(uuid: str) -> str:
    """Try to surface a friendly name (TONEX/AmpliTube/...) by reading the
    on-device registry.json. Best-effort — empty string on any failure."""
    try:
        out = subprocess.check_output(
            ["adb", "shell", "run-as", APP_PACKAGE, "cat", "files/vst_plugins/registry.json"],
            stderr=subprocess.DEVNULL,
        ).decode("utf-8", errors="replace")
        m = re.search(
            r'"uuid"\s*:\s*"' + re.escape(uuid) + r'".*?"displayName"\s*:\s*"([^"]+)"',
            out,
            re.S,
        )
        if m:
            return m.group(1)
    except Exception:
        pass
    return ""


# ---------------------------------------------------------------------------
# Entrypoint
# ---------------------------------------------------------------------------

def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description="Triage a vst_host_*.log into a 1-page health report."
    )
    ap.add_argument("path", nargs="?", help="Local log path; omit to use --adb")
    ap.add_argument(
        "--adb", metavar="UUID", help="Pull cache/vst_host_v<UUID>.log via adb run-as"
    )
    ap.add_argument("--full", action="store_true", help="Dump every counter, not just notable")
    args = ap.parse_args(argv)

    state = TriageState()
    started = time.monotonic()

    if args.adb:
        state.log_path = f"adb://cache/vst_host_v{args.adb}.log"
        state.log_size = adb_log_size(args.adb)
        name = plugin_name_from_registry(args.adb)
        if name:
            state.log_path += f"  (plugin={name})"
        src = stream_adb(args.adb)
    elif args.path:
        p = Path(args.path)
        if not p.exists():
            print(f"error: log file not found: {p}", file=sys.stderr)
            return 2
        state.log_path = str(p)
        state.log_size = p.stat().st_size
        src = stream_local(p)
    else:
        ap.print_help()
        return 2

    for lineno, line in enumerate(src, start=1):
        parse_line(state, line, lineno)

    refs = cross_reference(state, load_memory_index())
    print(report(state, args.full, refs))

    elapsed = time.monotonic() - started
    print(f"(triaged in {elapsed:.1f}s)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
