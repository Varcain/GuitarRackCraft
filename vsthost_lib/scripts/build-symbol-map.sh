#!/usr/bin/env bash
# Generate a flat symbol map for wine / DXVK / vst3_host binaries so the
# triage script (triage-vst-log.py) can resolve a crash address like
# "<module_base>+FE7784" to a function name.
#
# The wine ARM64 native crashes we hit (e.g. the TONEX downstream
# WRITE-NULL) show up in the VEH log as bare hex addresses. Without a
# symbol map an agent can only guess which wine module + function the
# fault is in. With this map, the triage script can report
# "ntdll.so::RtlpWaitForCriticalSection +0x84" instead of "<base>+FE7784".
#
# Output: src/main/assets/wine-symbol-map.txt
#   Format (one line per FUNC/TEXT symbol):
#     <module> <offset_hex> <symbol_name>
#   Sorted by (module, offset) so the triage script can bisect.
#
# This is OPTIONAL build output — a missing/failed symbol map only means
# crash addresses stay unresolved; it never breaks the APK build. Call
# from build-all.sh AFTER build-wine-android.sh + build-wine-pe.sh +
# build-vst3-host.sh have produced their binaries.

set -uo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/26.1.10909125}"
NM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm"
if [ ! -x "$NM" ]; then
    NM="$(command -v llvm-nm || true)"
fi
if [ -z "$NM" ] || [ ! -x "$NM" ]; then
    echo "WARN: llvm-nm not found — skipping symbol map (crash addrs stay unresolved)" >&2
    exit 0
fi

out="$repo_root/src/main/assets/wine-symbol-map.txt"
tmp="$(mktemp)"

wine_build="$repo_root/external/wine-upstream/build-android-arm64"
dxvk_build="$repo_root/external/dxvk/build-win64"
vst3_host="$repo_root/external/vst_host_vst3/build/vst3_host.exe"

# emit_syms BIN LABEL >> $tmp
# Dumps code symbols (T/t/W/w) as: "<label> <addr_hex> <name>".
emit_syms() {
    local bin="$1" label="$2"
    [ -f "$bin" ] || return 0
    "$NM" --defined-only "$bin" 2>/dev/null \
      | awk -v lbl="$label" '
          $2 ~ /^[TtWw]$/ && $1 ~ /^[0-9a-fA-F]+$/ && NF >= 3 {
            name = $3;
            for (i = 4; i <= NF; i++) name = name " " $i;
            print lbl, $1, name;
          }'
}

n_mod=0

# Group all emit_syms output into $tmp via a single redirect block.
{
    # 1. Bionic-side wine modules (.so) — native ARM64 code, our hardest
    #    crash sites (e.g. the TONEX downstream WRITE-NULL).
    if [ -d "$wine_build/dlls" ]; then
        while IFS= read -r so; do
            emit_syms "$so" "$(basename "$so")"
            n_mod=$((n_mod + 1))
        done < <(find "$wine_build/dlls" -maxdepth 2 -name '*.so' -type f)
        emit_syms "$wine_build/loader/wine" "wine"
        emit_syms "$wine_build/server/wineserver" "wineserver"
    fi

    # 2. PE-as-ELF wine modules (aarch64-windows/*.dll) — upper halves.
    if [ -d "$wine_build/dlls" ]; then
        while IFS= read -r dll; do
            emit_syms "$dll" "pe:$(basename "$dll")"
            n_mod=$((n_mod + 1))
        done < <(find "$wine_build/dlls" -path '*/aarch64-windows/*.dll' -type f)
    fi

    # 3. DXVK DLLs (x86_64 PE) — D3D11/DXGI crash sites.
    if [ -d "$dxvk_build" ]; then
        while IFS= read -r dll; do
            emit_syms "$dll" "dxvk:$(basename "$dll")"
            n_mod=$((n_mod + 1))
        done < <(find "$dxvk_build" -name '*.dll' -type f 2>/dev/null)
    fi

    # 4. The VST3 host itself.
    if [ -f "$vst3_host" ]; then
        emit_syms "$vst3_host" "vst3_host.exe"
        n_mod=$((n_mod + 1))
    fi
} > "$tmp"

# n_mod was incremented in a subshell (process substitution + while), so
# recompute the distinct module count from the output for the summary.
mod_count="$(awk '{print $1}' "$tmp" | LC_ALL=C sort -u | wc -l)"

# Sort by (module, numeric-hex offset). The triage script bisects within
# a module so it needs offsets in order. We can't numeric-sort hex with
# plain `sort -n`, so left-pad to 16 hex digits in a scratch column,
# sort, then drop the scratch column.
LC_ALL=C awk '{ printf "%s %016s %s %s\n", $1, $2, $2, $3 }' "$tmp" \
  | LC_ALL=C sort -k1,1 -k2,2 \
  | awk '{ $2=""; sub(/  /, " "); print }' \
  > "$out"

rm -f "$tmp"

if [ -s "$out" ]; then
    lines="$(wc -l < "$out")"
    size="$(du -h "$out" | cut -f1)"
    echo "[=] wine-symbol-map.txt: $lines symbols from $mod_count modules ($size) -> $out"
else
    echo "WARN: symbol map empty (no binaries found — did the wine/dxvk builds run?)" >&2
    rm -f "$out"
fi
