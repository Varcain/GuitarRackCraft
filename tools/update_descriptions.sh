#!/bin/bash
# =============================================================================
# tools/update_descriptions.sh — Extract plugin descriptions from source TTLs,
# READMEs, and category heuristics. Only fills in empty/missing entries in
# plugin_descriptions.json; never overwrites existing descriptions.
#
# Run manually when new plugins are added.
# =============================================================================

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
THIRD_PARTY="$PROJECT_ROOT/3rd_party"
DESC_FILE="$PROJECT_ROOT/plugin_descriptions.json"
ASSETS_DIR="$PROJECT_ROOT/app/src/main/assets/lv2"

python3 - "$THIRD_PARTY" "$DESC_FILE" "$ASSETS_DIR" << 'PYEOF'
import json, re, sys, os, glob

THIRD_PARTY = sys.argv[1]
DESC_FILE   = sys.argv[2]
ASSETS_DIR  = sys.argv[3]

# ── Load existing descriptions ──────────────────────────────────────────────

if os.path.exists(DESC_FILE):
    with open(DESC_FILE) as f:
        descriptions = json.load(f)
else:
    descriptions = {}

original = dict(descriptions)

# ── Helpers ─────────────────────────────────────────────────────────────────

def extract_comment(path):
    """Extract first paragraph of rdfs:comment from a TTL file."""
    try:
        text = open(path).read()
        # Try multiline """..."""
        m = re.search(r'rdfs:comment\s+"""(.*?)"""', text, re.DOTALL)
        if not m:
            m = re.search(r'rdfs:comment\s+"([^"]*)"', text)
        if m:
            c = m.group(1).strip()
            if c and c != "...":
                # Take first paragraph
                c = re.split(r'\n\n', c)[0]
                c = re.split(r'\nFeatures:', c)[0]
                c = re.split(r'\nBased on:', c)[0]
                c = re.split(r'\n\*', c)[0]
                c = re.split(r'\nsource:', c)[0]
                c = re.sub(r'\n+', ' ', c).strip()
                return c
    except:
        pass
    return None

def extract_comment_from_dir(d):
    """Try all TTL files in a directory."""
    for ttl in sorted(glob.glob(os.path.join(d, "*.ttl"))):
        base = os.path.basename(ttl)
        if base in ("manifest.ttl", "modgui.ttl", "modguis.ttl"):
            continue
        c = extract_comment(ttl)
        if c:
            return c
    return None

def parse_readme(path):
    """Extract name and description from README.md (first paragraph after # header)."""
    try:
        lines = open(path).read().replace('\r', '').split('\n')
        header = lines[0]
        m = re.match(r'^#\s*(\S+?)\.lv2', header)
        if not m:
            m = re.match(r'^#\s*(\S+)', header)
        if m:
            desc_lines = []
            for line in lines[1:]:
                line = line.strip()
                if not line or line.startswith('#') or line.startswith('![') or line.startswith('<'):
                    break
                desc_lines.append(line)
            return ' '.join(desc_lines).strip() if desc_lines else None
    except:
        pass
    return None

CATEGORY_DESCS = {
    "DistortionPlugin": "Distortion/overdrive effect",
    "AmplifierPlugin": "Amplifier simulation",
    "SimulatorPlugin": "Amp/cabinet simulator",
    "CompressorPlugin": "Dynamic range compressor",
    "DelayPlugin": "Delay effect",
    "ReverbPlugin": "Reverb effect",
    "ChorusPlugin": "Chorus effect",
    "FlangerPlugin": "Flanger effect",
    "PhaserPlugin": "Phaser effect",
    "FilterPlugin": "Filter/tone shaping effect",
    "EQPlugin": "Equalizer",
    "ModulatorPlugin": "Modulation effect",
    "GatePlugin": "Noise gate",
    "ExpanderPlugin": "Expander/gate",
    "UtilityPlugin": "Utility",
    "PitchPlugin": "Pitch shifting effect",
    "AnalyserPlugin": "Signal analyser",
    "MixerPlugin": "Mixer/routing utility",
}

def extract_names_and_category(d):
    """Extract doap:name values and lv2 category from TTL files in a directory."""
    names = []
    category = "Plugin"
    for ttl in glob.glob(os.path.join(d, "*.ttl")):
        base = os.path.basename(ttl)
        if base in ("manifest.ttl", "modgui.ttl", "modguis.ttl"):
            continue
        text = open(ttl).read()
        for m in re.finditer(r'doap:name\s+"([^"]+)"', text):
            names.append(m.group(1))
        cm = re.search(r'lv2:(\w+Plugin)', text)
        if cm:
            category = cm.group(1)
    return names, category

def set_if_empty(name, desc):
    """Set description only if the entry is missing or empty."""
    if desc and (name not in descriptions or not descriptions[name]):
        descriptions[name] = desc

# ── Process GxPlugins ───────────────────────────────────────────────────────

gx_dir = os.path.join(THIRD_PARTY, "GxPlugins.lv2.Android")
for bundle in sorted(glob.glob(os.path.join(gx_dir, "*.lv2"))):
    if not os.path.isdir(bundle):
        continue
    bundle_name = os.path.basename(bundle)
    plugin_name = bundle_name.replace(".lv2", "")

    # README
    desc = None
    readme = os.path.join(bundle, "README.md")
    if os.path.exists(readme):
        desc = parse_readme(readme)

    # TTL fallback
    if not desc:
        plugin_dir = os.path.join(bundle, "plugin")
        if os.path.isdir(plugin_dir):
            desc = extract_comment_from_dir(plugin_dir)

    # Category fallback
    _, cat = (extract_names_and_category(os.path.join(bundle, "plugin"))
              if os.path.isdir(os.path.join(bundle, "plugin")) else ([], "Plugin"))
    if not desc:
        desc = CATEGORY_DESCS.get(cat, "")

    set_if_empty(plugin_name, desc)

# ── Process Trunk Plugins ───────────────────────────────────────────────────

trunk_dir = os.path.join(THIRD_PARTY, "guitarix/trunk/src/LV2")
if os.path.isdir(trunk_dir):
    for bundle in sorted(glob.glob(os.path.join(trunk_dir, "*.lv2"))):
        if not os.path.isdir(bundle):
            continue
        desc = extract_comment_from_dir(bundle)
        names, cat = extract_names_and_category(bundle)

        if not desc:
            desc = CATEGORY_DESCS.get(cat, "")

        for name in names:
            set_if_empty(name, desc)

# ── Process External Plugins ────────────────────────────────────────────────

EXTERNALS = [
    ("neural_amp_modeler", ["neural-amp-modeler-lv2/resources"]),
    ("aidadsp",            ["aidadsp-lv2/rt-neural-generic/ttl"]),
    ("AIDA-X",             ["AIDA-X"]),
    ("ImpulseLoader",      ["ImpulseLoader/ImpulseLoader/lv2", "ImpulseLoader"]),
    ("XDarkTerror",        ["XDarkTerror/XDarkTerror/plugin", "XDarkTerror"]),
    ("XTinyTerror",        ["XTinyTerror/XTinyTerror/plugin", "XTinyTerror"]),
    ("CollisionDrive",     ["CollisionDrive/CollisionDrive", "CollisionDrive"]),
    ("MetalTone",          ["MetalTone/MetalTone", "MetalTone"]),
    ("GxCabSim",           ["GxCabSim.lv2/plugin", "GxCabSim.lv2"]),
    ("FatFrog",            ["FatFrog.lv2/FatFrog/plugin", "FatFrog.lv2"]),
    ("Neuralrack",         ["NeuralRack/NeuralRack/lv2", "NeuralRack"]),
    ("doubletracker",      ["doubletracker.lv2"]),
    ("PreAmps",            ["ModularAmpToolKit.lv2/PreAmps"]),
    ("PowerAmps",          ["ModularAmpToolKit.lv2/PowerAmps"]),
    ("PreAmpImpulses",     ["ModularAmpToolKit.lv2/PreAmpImpulses"]),
    ("PowerAmpImpulses",   ["ModularAmpToolKit.lv2/PowerAmpImpulses"]),
]

for dir_name, source_dirs in EXTERNALS:
    # Try assets TTL first
    assets_plugin = os.path.join(ASSETS_DIR, f"{dir_name}.lv2")
    desc = None
    if os.path.isdir(assets_plugin):
        desc = extract_comment_from_dir(assets_plugin)

    # Try source TTLs
    if not desc:
        for sd in source_dirs:
            src = os.path.join(THIRD_PARTY, sd)
            if os.path.isdir(src):
                desc = extract_comment_from_dir(src)
                if desc:
                    break

    # Try README
    if not desc:
        for sd in source_dirs:
            readme = os.path.join(THIRD_PARTY, sd, "README.md")
            if os.path.exists(readme):
                desc = parse_readme(readme)
                if desc:
                    break

    # Category fallback
    if not desc:
        if os.path.isdir(assets_plugin):
            _, cat = extract_names_and_category(assets_plugin)
            desc = CATEGORY_DESCS.get(cat, "")

    # Get all plugin names from the TTLs
    names = []
    if os.path.isdir(assets_plugin):
        names, _ = extract_names_and_category(assets_plugin)
    if not names:
        names = [dir_name]

    for name in names:
        set_if_empty(name, desc)

# ── Write output ────────────────────────────────────────────────────────────

added = [k for k in descriptions if k not in original or (not original.get(k) and descriptions[k])]
with open(DESC_FILE, 'w') as f:
    json.dump(dict(sorted(descriptions.items())), f, indent=2, ensure_ascii=False)
    f.write('\n')

print(f"Total: {len(descriptions)}, Updated: {len(added)}")
for k in sorted(added):
    print(f"  + {k}: {descriptions[k][:80]}")
PYEOF
