#!/bin/bash
# =============================================================================
# tools/generate_thumbnails.sh — Automated LV2 plugin thumbnail generation
#
# Builds plugins natively, renders their X11 UIs in a virtual framebuffer,
# and captures screenshots for use as plugin browser thumbnails.
#
# Requirements: Xvfb, jackd, jalv.gtk3, xdotool, imagemagick (convert), xwd
# =============================================================================

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
THIRD_PARTY="$PROJECT_ROOT/3rd_party"
ASSETS_DIR="$PROJECT_ROOT/app/src/main/assets/lv2"
TRUNK_LV2="$THIRD_PARTY/guitarix/trunk/src/LV2"

BUILD_DIR="/tmp/lv2_thumb_build"
LV2_DIR="/tmp/lv2_thumb_plugins"
SCREENSHOTS_DIR="$PROJECT_ROOT/tools/screenshots"

DISPLAY_NUM=99
XVFB_PID=""
JACK_PID=""
TRUNK_RES_OBJECTS=""

# ─── Helpers ─────────────────────────────────────────────────────────────────

cleanup() {
    [ -n "$JACK_PID" ] && kill "$JACK_PID" 2>/dev/null || true
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

log()  { echo -e "\033[1;34m[thumb]\033[0m $*"; }
warn() { echo -e "\033[1;33m[warn]\033[0m $*"; }
err()  { echo -e "\033[1;31m[err]\033[0m $*"; }

check_deps() {
    local missing=()
    for cmd in Xvfb jackd jalv.gtk3 xdotool convert xwd g++ pkg-config; do
        command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
    done
    if [ ${#missing[@]} -gt 0 ]; then
        err "Missing tools: ${missing[*]}"
        err "Install with: sudo apt-get install -y xvfb jackd2 jalv xdotool imagemagick x11-apps build-essential pkg-config"
        exit 1
    fi
}

start_services() {
    log "Starting Xvfb on :$DISPLAY_NUM"
    Xvfb ":$DISPLAY_NUM" -screen 0 1280x1024x24 -ac >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 1

    export DISPLAY=":$DISPLAY_NUM"

    log "Starting JACK dummy driver"
    jackd --no-realtime -d dummy -r 48000 -p 1024 >/dev/null 2>&1 &
    JACK_PID=$!
    sleep 2
}

# ─── Screenshot capture ─────────────────────────────────────────────────────

capture_plugin() {
    local uri="$1"
    local name="$2"
    local output_dir="$3"

    export LV2_PATH="$LV2_DIR"

    # Launch jalv with the plugin
    jalv.gtk3 -s 48000 "$uri" >/dev/null 2>&1 &
    local jalv_pid=$!
    sleep 3

    # Find the plugin window
    local win_id
    win_id=$(xdotool search --pid "$jalv_pid" 2>/dev/null | tail -1)

    if [ -z "$win_id" ]; then
        # Fallback: search by name
        win_id=$(xdotool search --name "$name" 2>/dev/null | head -1)
    fi

    if [ -z "$win_id" ]; then
        warn "No window found for $name ($uri)"
        kill "$jalv_pid" 2>/dev/null || true
        wait "$jalv_pid" 2>/dev/null || true
        return 1
    fi

    # Capture the window
    mkdir -p "$output_dir"
    local raw="/tmp/thumb_${name}_raw.xwd"
    local screenshot="$output_dir/screenshot-${name}.png"
    local thumbnail="$output_dir/thumbnail-${name}.png"

    xwd -id "$win_id" -out "$raw" 2>/dev/null

    # Convert: crop jalv menu bar (27px), save screenshot
    convert "$raw" -crop +0+27 +repage "$screenshot" 2>/dev/null

    # Create thumbnail (256px wide, proportional)
    convert "$screenshot" -resize 256x "$thumbnail" 2>/dev/null

    rm -f "$raw"

    local dims
    dims=$(identify -format "%wx%h" "$screenshot" 2>/dev/null)
    log "  Captured $name: $dims -> $screenshot"

    kill "$jalv_pid" 2>/dev/null || true
    wait "$jalv_pid" 2>/dev/null || true
    sleep 1
    return 0
}

# ─── Build: Brummer plugins ─────────────────────────────────────────────────

build_brummer_plugin() {
    local src_dir="$1"
    local bundle_name="$2"

    log "Building $bundle_name (brummer Makefile)"
    cd "$src_dir"
    make clean >/dev/null 2>&1 || true
    if make -j"$(nproc)" >/dev/null 2>&1; then
        make INSTALL_DIR="$LV2_DIR" install >/dev/null 2>&1
        log "  Built and installed $bundle_name"
        cd "$PROJECT_ROOT"
        return 0
    else
        warn "  Failed to build $bundle_name"
        cd "$PROJECT_ROOT"
        return 1
    fi
}

# ─── Build: xputty static library (shared by all trunk plugins) ──────────────

build_xputty() {
    local xputty="$TRUNK_LV2/xputty"
    local cairo_flags
    cairo_flags=$(pkg-config --cflags cairo x11)
    local objects=""

    log "Building xputty static library..."

    # Compile widget sources
    for f in "$xputty"/widgets/*.cpp; do
        local base obj
        base=$(basename "${f%.cpp}")
        obj="$BUILD_DIR/xputty_${base}.o"
        if g++ -O2 -fPIC $cairo_flags \
            -I"$xputty/header" -I"$xputty/header/widgets" -I"$xputty/header/dialogs" \
            -I"$xputty/resources" -I"$xputty/xdgmime" \
            -c "$f" -o "$obj" 2>/dev/null; then
            objects="$objects $obj"
        fi
    done

    # Compile xdgmime
    for f in "$xputty"/xdgmime/*.c; do
        local base obj
        base=$(basename "${f%.c}")
        obj="$BUILD_DIR/xputty_${base}.o"
        if gcc -O2 -fPIC -I"$xputty/xdgmime" -c "$f" -o "$obj" 2>/dev/null; then
            objects="$objects $obj"
        fi
    done

    # Compile core xputty sources (xwidget, xputty, xcolor, etc.)
    for f in "$xputty"/*.cpp; do
        local base obj
        base=$(basename "${f%.cpp}")
        obj="$BUILD_DIR/xputty_${base}.o"
        if g++ -O2 -fPIC $cairo_flags \
            -I"$xputty/header" -I"$xputty/header/widgets" -I"$xputty/header/dialogs" \
            -I"$xputty/resources" -I"$xputty/xdgmime" \
            -c "$f" -o "$obj" 2>/dev/null; then
            objects="$objects $obj"
        fi
    done

    # Compile dialogs
    for f in "$xputty"/dialogs/*.cpp; do
        local base obj
        base=$(basename "${f%.cpp}")
        obj="$BUILD_DIR/xputty_${base}.o"
        if g++ -O2 -fPIC $cairo_flags \
            -I"$xputty/header" -I"$xputty/header/widgets" -I"$xputty/header/dialogs" \
            -I"$xputty/resources" -I"$xputty/xdgmime" \
            -c "$f" -o "$obj" 2>/dev/null; then
            objects="$objects $obj"
        fi
    done

    ar rcs "$BUILD_DIR/libxputty_trunk.a" $objects 2>/dev/null
    log "  xputty: $(echo $objects | wc -w) objects"

    # Embed PNG resources (must cd so symbol names are just filenames)
    TRUNK_RES_OBJECTS=""
    local _orig_dir
    _orig_dir=$(pwd)
    cd "$xputty/resources"
    for png in *.png; do
        local base obj
        base="${png%.png}"
        obj="$BUILD_DIR/res_${base}.o"
        ld -r -b binary -o "$obj" "$png" 2>/dev/null
        TRUNK_RES_OBJECTS="$TRUNK_RES_OBJECTS $obj"
    done
    cd "$_orig_dir"
    log "  resources: $(echo $TRUNK_RES_OBJECTS | wc -w) PNGs embedded"
}

# ─── Build: Trunk plugin (stub DSP + full UI with xputty) ───────────────────

build_trunk_plugin() {
    local plugin_dir="$1"        # e.g., gx_bmp.lv2
    local plugin_base="$2"       # e.g., gx_bmp
    local plugin_uri_full="$3"   # full URI from manifest

    local src="$TRUNK_LV2/$plugin_dir"
    local xputty="$TRUNK_LV2/xputty"
    local faust="$TRUNK_LV2/faust-generated"

    log "Building $plugin_dir (trunk stub+UI)"

    # Find UI source
    local ui_src
    ui_src=$(find "$src" -name "*_ui.cpp" 2>/dev/null | head -1)
    if [ -z "$ui_src" ]; then
        warn "  No UI source found for $plugin_dir"
        return 1
    fi

    local build="$BUILD_DIR/$plugin_dir"
    mkdir -p "$build"

    # Find the header that defines GXPLUGIN_URI and PortIndex
    local header
    header=$(find "$src" -name "*.h" ! -name "gx_common.h" 2>/dev/null | head -1)
    if [ -z "$header" ]; then
        warn "  No header found for $plugin_dir"
        return 1
    fi

    local header_name
    header_name=$(basename "$header")

    # Count ports from the PortIndex enum
    local num_ports
    num_ports=$(grep -cE "^\s+[A-Z_]+," "$header" 2>/dev/null || true)
    : "${num_ports:=5}"
    num_ports=$((num_ports + 1))  # account for last entry without comma

    # Create DSP stub with explicit URI (avoids header guard collisions)
    cat > "$build/stub_dsp.cpp" << STUBEOF
#include <lv2.h>
#include <cstdlib>
#include <cstring>
struct StubPlugin { float *ports[$num_ports]; };
static LV2_Handle inst(const LV2_Descriptor*, double, const char*, const LV2_Feature* const*) {
    return (LV2_Handle)calloc(1, sizeof(StubPlugin));
}
static void conn(LV2_Handle h, uint32_t p, void *d) {
    if (p < $num_ports) ((StubPlugin*)h)->ports[p] = (float*)d;
}
static void run(LV2_Handle h, uint32_t n) {
    StubPlugin *s = (StubPlugin*)h;
    if (s->ports[0] && s->ports[1]) memcpy(s->ports[0], s->ports[1], n*sizeof(float));
}
static void clean(LV2_Handle h) { free(h); }
static const LV2_Descriptor desc = { "$plugin_uri_full", inst, conn, NULL, run, NULL, clean, NULL };
extern "C" __attribute__((visibility("default")))
const LV2_Descriptor* lv2_descriptor(uint32_t i) { return i==0 ? &desc : NULL; }
STUBEOF

    # Compile DSP stub (no plugin header needed - URI is inline)
    if ! g++ -O2 -fPIC -shared -o "$build/${plugin_base}.so" "$build/stub_dsp.cpp" \
        $(pkg-config --cflags lv2) -lm 2>/dev/null; then
        warn "  DSP stub failed for $plugin_dir"
        return 1
    fi

    # Compile real UI with xputty + resources
    if ! g++ -O2 -fPIC -shared -o "$build/${plugin_base}_ui.so" "$ui_src" \
        -I"$src" \
        -I"$xputty/header" -I"$xputty/header/widgets" -I"$xputty/header/dialogs" \
        -I"$xputty/resources" -I"$xputty/lv2_plugin" -I"$xputty/xdgmime" \
        -I"$faust" \
        $(pkg-config --cflags --libs lv2 cairo x11) \
        -Wl,--whole-archive "$BUILD_DIR/libxputty_trunk.a" -Wl,--no-whole-archive \
        $TRUNK_RES_OBJECTS \
        -lm -fvisibility=hidden 2>/dev/null; then
        warn "  UI build failed for $plugin_dir"
        return 1
    fi

    # Install to LV2 path
    local dest="$LV2_DIR/$plugin_dir"
    mkdir -p "$dest"
    cp "$build/${plugin_base}.so" "$build/${plugin_base}_ui.so" "$dest/"

    # Copy TTL from assets
    local assets_src="$ASSETS_DIR/GxPlugins.lv2/$plugin_dir"
    if [ -d "$assets_src" ]; then
        cp "$assets_src"/*.ttl "$dest/" 2>/dev/null || true
    fi

    log "  Built and installed $plugin_dir"
    return 0
}

# ─── Capture trunk plugin via custom host ────────────────────────────────────

capture_trunk_plugin() {
    local dir="$1" base="$2" uri="$3" display="$4" output_dir="$5"

    local ui_so="$LV2_DIR/$dir/${base}_ui.so"
    local bundle="$LV2_DIR/$dir/"

    if [ ! -f "$ui_so" ]; then
        warn "No UI .so for $dir"
        return 1
    fi

    # Get UI URI from the plugin TTL (look for guiext:ui <URI>)
    local ui_uri
    ui_uri=$(grep "guiext:ui" "$LV2_DIR/$dir/"*.ttl 2>/dev/null | grep -o '<[^>]*>' | head -1 | tr -d '<>')
    if [ -z "$ui_uri" ]; then
        # Fallback: look for X11UI
        ui_uri=$(grep -B1 "X11UI" "$LV2_DIR/$dir/"*.ttl 2>/dev/null | grep -o '<[^>]*>' | head -1 | tr -d '<>')
    fi
    if [ -z "$ui_uri" ]; then
        ui_uri="${uri}_gui"
    fi

    mkdir -p "$output_dir"
    local ppm="/tmp/thumb_${display}.ppm"
    local screenshot="$output_dir/screenshot-${display}.png"
    local thumbnail="$output_dir/thumbnail-${display}.png"

    local dims
    dims=$("$BUILD_DIR/lv2_screenshot" "$ui_so" "$uri" "$ui_uri" "$bundle" "$ppm" 2>/dev/null)

    if [ -f "$ppm" ] && [ -s "$ppm" ]; then
        convert "$ppm" "$screenshot" 2>/dev/null
        convert "$screenshot" -resize 256x "$thumbnail" 2>/dev/null
        rm -f "$ppm"

        # Verify non-black capture
        local mean
        mean=$(convert "$screenshot" -format '%[fx:mean]' info: 2>/dev/null)
        if [ "$(echo "$mean > 0.01" | bc -l 2>/dev/null)" = "1" ] 2>/dev/null; then
            log "  Captured $display: $dims -> $screenshot"
            return 0
        else
            warn "  Capture was black for $dir ($display)"
            rm -f "$screenshot" "$thumbnail"
            return 1
        fi
    else
        rm -f "$ppm"
        warn "  Screenshot failed for $dir ($display)"
        return 1
    fi
}

# ─── Plugin definitions ─────────────────────────────────────────────────────

declare -A BRUMMER_PLUGINS=(
    ["FatFrog.lv2"]="$THIRD_PARTY/FatFrog.lv2"
    ["XDarkTerror"]="$THIRD_PARTY/XDarkTerror"
    ["XTinyTerror"]="$THIRD_PARTY/XTinyTerror"
    ["GxCabSim.lv2"]="$THIRD_PARTY/GxCabSim.lv2"
)

# plugin_dir|plugin_base|full_uri|display_name
TRUNK_PLUGINS=(
    "gx_aclipper.lv2|gx_aclipper|http://guitarix.sourceforge.net/plugins/gx_aclipper_#_aclipper_|aclipper"
    "gx_bmp.lv2|gx_bmp|http://guitarix.sourceforge.net/plugins/gx_bmp_#_bmp_|bmp"
    "gx_bossds1.lv2|gx_bossds1|http://guitarix.sourceforge.net/plugins/gx_bossds1_#_bossds1_|bossds1"
    "gx_gcb_95.lv2|gx_gcb_95|http://guitarix.sourceforge.net/plugins/gx_gcb_95_#_gcb_95_|gcb95"
    "gx_mole.lv2|gx_mole|http://guitarix.sourceforge.net/plugins/gx_mole_#_mole_|mole"
    "gx_muff.lv2|gx_muff|http://guitarix.sourceforge.net/plugins/gx_muff_#_muff_|muff"
    "gx_mxrdist.lv2|gx_mxrdist|http://guitarix.sourceforge.net/plugins/gx_mxrdist_#_mxrdist_|mxrdist"
    "gx_w20.lv2|gx_w20|http://guitarix.sourceforge.net/plugins/gx_w20#w20|w20"
    "gx_zita_rev1.lv2|gx_zita_rev1|http://guitarix.sourceforge.net/plugins/gx_zita_rev1_stereo#_zita_rev1_stereo|zitarev1"
    "gxmetal_amp.lv2|gxmetal_amp|http://guitarix.sourceforge.net/plugins/gxmetal_amp#metal_amp|metalamp"
    "gxmetal_head.lv2|gxmetal_head|http://guitarix.sourceforge.net/plugins/gxmetal_head#metal_head|metalhead"
    "gxtape.lv2|gxtape|http://guitarix.sourceforge.net/plugins/gxtape#tape|tape"
    "gxtape_st.lv2|gxtape_st|http://guitarix.sourceforge.net/plugins/gxtape_st#tape|tapest"
    "gxts9.lv2|gxts9|http://guitarix.sourceforge.net/plugins/gxts9#ts9sim|ts9"
    "gxtuner.lv2|gxtuner|http://guitarix.sourceforge.net/plugins/gxtuner#tuner|tuner"
)

# Brummer plugin URIs (from their manifest.ttl files)
declare -A BRUMMER_URIS=(
    ["FatFrog.lv2"]="https://github.com/brummer10/FatFrog#_FatFrog_"
    ["XDarkTerror.lv2"]="http://guitarix.sourceforge.net/plugins/XDarkTerror_#_darkterror_"
    ["XTinyTerror.lv2"]="http://guitarix.sourceforge.net/plugins/XTinyTerror_#_tinyterror_"
    ["gx_cabsim.lv2"]="http://guitarix.sourceforge.net/plugins/gx_cabsim_#_cabsim_"
)

# ─── Main ────────────────────────────────────────────────────────────────────

main() {
    check_deps

    rm -rf "$BUILD_DIR" "$LV2_DIR"
    mkdir -p "$BUILD_DIR" "$LV2_DIR" "$SCREENSHOTS_DIR"

    log "Phase 1: Building plugins natively"

    # Build brummer plugins
    for bundle in "${!BRUMMER_PLUGINS[@]}"; do
        build_brummer_plugin "${BRUMMER_PLUGINS[$bundle]}" "$bundle" || true
    done

    # Build xputty (shared by all trunk plugins)
    build_xputty

    # Build trunk plugins
    for entry in "${TRUNK_PLUGINS[@]}"; do
        IFS='|' read -r dir base uri display <<< "$entry"
        build_trunk_plugin "$dir" "$base" "$uri" || true
    done

    # Build the custom screenshot host
    log "Building lv2_screenshot host..."
    gcc -O2 -o "$BUILD_DIR/lv2_screenshot" "$PROJECT_ROOT/tools/lv2_screenshot.c" \
        $(pkg-config --cflags --libs x11 lv2) -ldl 2>/dev/null

    log "Phase 2: Starting virtual display and audio"
    start_services

    log "Phase 3: Capturing screenshots"
    local captured=0
    local failed=0

    # Capture brummer plugins (via jalv — they have full DSP+UI)
    for bundle in "${!BRUMMER_URIS[@]}"; do
        local uri="${BRUMMER_URIS[$bundle]}"
        local name
        name=$(echo "$bundle" | sed 's/\.lv2$//' | tr '[:upper:]' '[:lower:]')
        local dest="$ASSETS_DIR/${bundle}"

        if [ -d "$LV2_DIR/$bundle" ]; then
            if capture_plugin "$uri" "$name" "$dest"; then
                captured=$((captured + 1))
            else
                failed=$((failed + 1))
            fi
        fi
    done

    # Capture trunk plugins (via custom lv2_screenshot host for proper X11 UI)
    for entry in "${TRUNK_PLUGINS[@]}"; do
        IFS='|' read -r dir base uri display <<< "$entry"
        local dest="$ASSETS_DIR/GxPlugins.lv2/$dir"

        if [ -d "$LV2_DIR/$dir" ]; then
            if capture_trunk_plugin "$dir" "$base" "$uri" "$display" "$dest"; then
                captured=$((captured + 1))
            else
                failed=$((failed + 1))
            fi
        fi
    done

    log "Done! Captured: $captured, Failed: $failed"
    log "Screenshots saved to asset directories"
    log "Re-run cmake configure to regenerate plugin_metadata.json"
}

main "$@"
