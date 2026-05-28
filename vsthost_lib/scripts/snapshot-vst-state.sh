#!/usr/bin/env bash
# Capture EVERYTHING an agent typically gathers via half-a-dozen separate
# `adb shell` calls when debugging a failing VST plugin. Output is a
# single text file with named sections.
#
# Sections:
#   1. Device process state          (guitarrackcraft / wineserver / vst3_host)
#   2. Wineserver state              (alive / dead / parent)
#   3. X11 listeners + orphans       (ports 6000–6099 with backlog flagging)
#   4. Plugin prefixes               (registry.json walk, DXVK install state,
#                                     user.reg DllOverrides, .no-dxvk marker)
#   5. FEX AOT cache size
#   6. Recent log files              (size + mtime)
#   7. Per-plugin triage             (calls triage-vst-log.py on the
#                                     freshest log per plugin)
#
# Usage:
#   bash snapshot-vst-state.sh                  # to /tmp/vst-state-<ts>.txt
#   bash snapshot-vst-state.sh > snapshot.txt   # to stdout
#
# Designed for an agent (or human) to take a single snapshot, paste the
# output, and have a complete picture of "what is the device doing right
# now" without spelunking. Runtime ~3-5 s.

set -uo pipefail

APP=com.varcain.guitarrackcraft
script_dir="$(cd "$(dirname "$0")" && pwd)"
triage="$script_dir/triage-vst-log.py"

# If invoked without redirection, write to a timestamped file.
if [ -t 1 ]; then
    ts="$(date +%Y%m%d-%H%M%S)"
    out="/tmp/vst-state-${ts}.txt"
    exec > >(tee "$out")
    echo "# Output also tee'd to $out" >&2
fi

now="$(date '+%Y-%m-%d %H:%M:%S %z')"

adb_run() {
    # Wrapper so a missing adb / no-device fails ONE section, not whole script.
    adb shell "$@" 2>&1
}

adb_runas() {
    # `run-as` execs the target directly with no shell expansion. Wrap in
    # `sh -c` so globs (files/wineprefix_v*) and redirections work as
    # callers expect.
    adb shell "run-as $APP sh -c '$*'" 2>&1
}

print_header() {
    printf '\n=== %s ===\n' "$1"
}

print_header "snapshot @ $now"
echo "Package: $APP"
adb_run getprop ro.product.model 2>/dev/null | head -1 | xargs -I{} echo "Device: {}"
adb_run getprop ro.build.version.release 2>/dev/null | head -1 | xargs -I{} echo "Android: {}"

print_header "1. Device process state"
adb_run "ps -A" | awk 'NR==1 || /guitarrack|wineserver|vst3_host|vst_host/'

print_header "2. Wineserver state"
ws_line="$(adb_run "ps -A" | awk '/wineserver/ {print; exit}')"
if [ -n "$ws_line" ]; then
    ws_pid="$(echo "$ws_line" | awk '{print $2}')"
    ws_ppid="$(echo "$ws_line" | awk '{print $3}')"
    echo "ALIVE  pid=$ws_pid ppid=$ws_ppid"
    if [ "$ws_ppid" = "1" ]; then
        echo "       (re-parented to init → app force-stopped but wineserver lingers)"
    fi
else
    echo "DEAD   (clean state — fresh launch will re-spawn)"
fi

print_header "3. X11 listeners (ports 6000–6099)"
echo "Format: port → display (backlog)  status"
adb_run "ss -tln" 2>/dev/null | awk '/127\.0\.0\.1:60[0-9][0-9]/ {
    split($4, a, ":"); port=a[2]; display=port-6000;
    backlog=$2;
    status = (backlog > 0) ? "  ! ORPHAN (no client, has pending connects)" : "  OK";
    if (port >= 6000 && port <= 6099) printf "  %s → display %d (backlog=%s)%s\n", port, display, backlog, status;
}'
echo "(If any line shows ORPHAN: the listener has no owning process but the"
echo " kernel still holds the socket. Next wine subprocess that tries to"
echo " connect lands in its accept backlog. Reboot is the only reliable fix"
echo " — see [[tonex-vst3-editor-stall]].)"

print_header "4. Plugin prefixes"
echo "Registry (uuid → displayName, is64Bit, format):"
registry="$(adb_runas cat files/vst_plugins/registry.json 2>/dev/null)"
if [ -n "$registry" ]; then
    # Stage the JSON in a temp file so we don't run into nested-heredoc
    # surprises (bash treats inner heredocs literally inside other
    # heredocs).
    reg_tmp="$(mktemp)"
    printf '%s' "$registry" > "$reg_tmp"
    python3 - "$reg_tmp" <<'PYEOF'
import json, sys
try:
    with open(sys.argv[1]) as f:
        j = json.load(f)
    for p in j.get("plugins", []):
        uuid = p.get("uuid", "?")
        name = p.get("displayName", "?")
        fmt  = p.get("format", "?")
        bits = "64" if p.get("is64Bit") else "32"
        print("  " + uuid + "  " + name + "  (" + fmt + " " + bits + ")")
except Exception as e:
    print("  (parse error: " + str(e) + ")")
PYEOF
    rm -f "$reg_tmp"
else
    echo "  (no registry.json — no imported plugins)"
fi

echo ""
echo "Per-prefix DXVK/user.reg state:"
prefixes="$(adb_runas "ls -d files/wineprefix_v* 2>/dev/null" || echo "")"
for pfx in $prefixes; do
    # pfx like "files/wineprefix_v106ffc00-..."
    [ -z "$pfx" ] && continue
    uuid="${pfx#*wineprefix_v}"
    short_uuid="${uuid:0:8}"
    # name from registry — quick grep+sed extract rather than re-parsing JSON
    name="$(printf '%s' "$registry" | grep -o "\"uuid\":\"$uuid\"[^}]*\"displayName\":\"[^\"]*\"" | sed 's/.*"displayName":"\([^"]*\)".*/\1/')"
    [ -z "$name" ] && name="(orphan prefix)"

    echo "  $short_uuid  $name"

    # d3d11.dll size
    d3d_size="$(adb_runas "stat -c %s $pfx/drive_c/windows/system32/d3d11.dll" 2>/dev/null | head -1)"
    case "$d3d_size" in
        5353472|537[0-9]*) echo "    d3d11.dll: $d3d_size B  (DXVK 2.5.3)" ;;
        ""|*"No such"*)    echo "    d3d11.dll: missing" ;;
        *)                  echo "    d3d11.dll: $d3d_size B  (wine builtin or other)" ;;
    esac

    # .no-dxvk marker
    no_dxvk="$(adb_runas "test -e $pfx/.no-dxvk && echo yes" 2>/dev/null | head -1)"
    if [ "$no_dxvk" = "yes" ]; then
        echo "    .no-dxvk:  PRESENT (ensurePluginPrefix will skip DXVK install)"
    fi

    # user.reg DllOverride for d3d11/dxgi — extract via cat+grep without
    # single quotes (those collide with `sh -c '$*'` quoting).
    overrides="$(adb_runas cat $pfx/user.reg 2>/dev/null | grep -E '^"(d3d11|dxgi|d3d9)"' | head -3)"
    if [ -n "$overrides" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] && echo "    $line"
        done <<< "$overrides"
    fi

    # marker v7 (wine strips it on rewrite; absence = re-seed next play)
    marker_present="$(adb_runas cat $pfx/user.reg 2>/dev/null | grep -c vstpoc-d3d-overrides-v7 | head -1)"
    if [ "$marker_present" = "0" ]; then
        echo "    marker:    absent (next play will re-seed seedDisableDirect3D)"
    fi
done

print_header "5. FEX AOT cache"
adb_runas "du -sh cache/fex_aot 2>/dev/null; ls cache/fex_aot 2>/dev/null | wc -l" \
  | awk 'NR==1 {size=$1} NR==2 {print "  Size: " size "   files: " $1}'

print_header "6. Recent vst_host log files"
adb_runas "ls -lt cache/vst_host_*.log 2>/dev/null | head -6" \
  | awk '/vst_host/ {printf "  %12d B   %s %s   %s\n", $5, $6, $7, $NF}'

print_header "7. Per-plugin triage (diagnosis only)"
if [ ! -x "$triage" ]; then
    echo "  (triage script not found at $triage — skipping)"
else
    # Pull the freshest log for each plugin uuid that has a recent log.
    # Limit to top 3 by mtime to keep snapshot quick.
    logs="$(adb_runas "ls -t cache/vst_host_v*.log 2>/dev/null | head -3" | grep -E 'vst_host_v')"
    for log in $logs; do
        # Extract uuid from path: vst_host_v<UUID>.log
        bn="$(basename "$log" .log)"
        uuid="${bn#vst_host_v}"
        short="${uuid:0:8}"
        printf "  --- %s ---\n" "$short"
        # Run triage and extract only the Diagnosis section to keep the
        # snapshot compact. Full triage is one command away if needed.
        python3 "$triage" --adb "$uuid" 2>/dev/null \
          | awk '/^Diagnosis/ {flag=1} flag {print "    " $0}' \
          | head -8 || echo "    (triage failed)"
    done
fi

print_header "end of snapshot"
echo "For full per-plugin triage:  python3 $triage --adb <uuid>"
