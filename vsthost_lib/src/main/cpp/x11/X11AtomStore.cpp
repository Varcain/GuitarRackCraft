/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

#include "X11AtomStore.h"

namespace guitarrackcraft {

// X.org predefined atoms — atom IDs 1..68 are reserved by the X11 protocol
// and hardcoded in <X11/Xatom.h> (XA_PRIMARY=1 .. XA_WM_TRANSIENT_FOR=68).
// Wine's libX11 references these by hardcoded ID, so the X server MUST
// have them pre-registered or wine's first XChangeProperty(WM_NAME=39,...)
// silently fails and wine falls into a probe loop.
//
// Order matches au.com.darkside.xserver.Atom._predefinedAtoms (which is
// the X.org standard order).
static const char* kPredefinedAtoms[] = {
    "PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP",
    "CARDINAL", "COLORMAP", "CURSOR",
    "CUT_BUFFER0", "CUT_BUFFER1", "CUT_BUFFER2", "CUT_BUFFER3",
    "CUT_BUFFER4", "CUT_BUFFER5", "CUT_BUFFER6", "CUT_BUFFER7",
    "DRAWABLE", "FONT", "INTEGER", "PIXMAP", "POINT", "RECTANGLE",
    "RESOURCE_MANAGER", "RGB_COLOR_MAP", "RGB_BEST_MAP",
    "RGB_BLUE_MAP", "RGB_DEFAULT_MAP", "RGB_GRAY_MAP",
    "RGB_GREEN_MAP", "RGB_RED_MAP", "STRING", "VISUALID",
    "WINDOW", "WM_COMMAND", "WM_HINTS", "WM_CLIENT_MACHINE",
    "WM_ICON_NAME", "WM_ICON_SIZE", "WM_NAME",
    "WM_NORMAL_HINTS", "WM_SIZE_HINTS", "WM_ZOOM_HINTS",
    "MIN_SPACE", "NORM_SPACE", "MAX_SPACE", "END_SPACE",
    "SUPERSCRIPT_X", "SUPERSCRIPT_Y", "SUBSCRIPT_X", "SUBSCRIPT_Y",
    "UNDERLINE_POSITION", "UNDERLINE_THICKNESS",
    "STRIKEOUT_ASCENT", "STRIKEOUT_DESCENT", "ITALIC_ANGLE",
    "X_HEIGHT", "QUAD_WIDTH", "WEIGHT", "POINT_SIZE",
    "RESOLUTION", "COPYRIGHT", "NOTICE", "FONT_NAME",
    "FAMILY_NAME", "FULL_NAME", "CAP_HEIGHT",
    "WM_CLASS", "WM_TRANSIENT_FOR",
};

X11AtomStore::X11AtomStore() {
    for (uint32_t i = 0; i < sizeof(kPredefinedAtoms) / sizeof(kPredefinedAtoms[0]); ++i) {
        const uint32_t id = i + 1;
        nameToId_[kPredefinedAtoms[i]] = id;
        idToName_[id] = kPredefinedAtoms[i];
    }
    // Java server also has CLIPBOARD as predefined #3; X.org standard
    // doesn't, but wine asks for it early. Add as the first non-predefined
    // atom so subsequent custom atoms come after.
    nextId_ = sizeof(kPredefinedAtoms) / sizeof(kPredefinedAtoms[0]) + 1;
}

uint32_t X11AtomStore::intern(const std::string& name, bool onlyIfExists) {
    auto it = nameToId_.find(name);
    if (it != nameToId_.end()) {
        return it->second;
    }
    if (!onlyIfExists && !name.empty()) {
        uint32_t id = nextId_++;
        nameToId_[name] = id;
        idToName_[id] = name;
        return id;
    }
    return 0; // None
}

std::string X11AtomStore::getName(uint32_t atomId) const {
    auto it = idToName_.find(atomId);
    if (it != idToName_.end()) {
        return it->second;
    }
    return {};
}

void X11AtomStore::clear() {
    nameToId_.clear();
    idToName_.clear();
    nextId_ = 1;
    // Re-register predefined atoms so their X.org IDs (1..68) survive
    // across reconnects.
    for (uint32_t i = 0; i < sizeof(kPredefinedAtoms) / sizeof(kPredefinedAtoms[0]); ++i) {
        const uint32_t id = i + 1;
        nameToId_[kPredefinedAtoms[i]] = id;
        idToName_[id] = kPredefinedAtoms[i];
    }
    nextId_ = sizeof(kPredefinedAtoms) / sizeof(kPredefinedAtoms[0]) + 1;
}

} // namespace guitarrackcraft
