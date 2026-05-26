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

#pragma once

#include <cstdint>

namespace guitarrackcraft {

// X11 connection constants
static constexpr uint8_t kX11ConnectionAccepted = 1;
static constexpr uint16_t kX11Major = 11;
static constexpr uint16_t kX11Minor = 0;
// X11 server-resource IDs. All resource IDs (windows, colormaps, pixmaps,
// GCs, visuals, fonts) share one namespace; they must be unique AND
// below resource_id_base (0x00100000) so they don't collide with
// client-allocated IDs.
// Match the Java X server (au.com.darkside.xserver) byte-for-byte. Java
// allocates root=3, colormap=4, visual=1 — distinct resource IDs from a
// single namespace. Previously we used 1/1/0x21, but the colliding
// root+colormap pair plus a non-1 visual made some wine probes diverge
// from the validated Java path. Now matched exactly.
static constexpr uint32_t kRootWindowId      = 3;
static constexpr uint32_t kDefaultColormapId = 4;
static constexpr uint32_t kDefaultVisualId   = 1;
static constexpr uint32_t kWhitePixel = 0xffffffff;
static constexpr uint32_t kBlackPixel = 0xff000000;

// X11 request opcodes
namespace X11Op {
    static constexpr uint8_t CreateWindow = 1;
    static constexpr uint8_t ChangeWindowAttributes = 2;
    static constexpr uint8_t GetWindowAttributes = 3;
    static constexpr uint8_t DestroyWindow = 4;
    static constexpr uint8_t MapWindow = 8;
    static constexpr uint8_t UnmapWindow = 10;
    static constexpr uint8_t ConfigureWindow = 12;
    static constexpr uint8_t GetGeometry = 14;
    static constexpr uint8_t InternAtom = 16;
    static constexpr uint8_t GetAtomName = 17;
    static constexpr uint8_t ChangeProperty = 18;
    static constexpr uint8_t DeleteProperty = 19;
    static constexpr uint8_t GetProperty = 20;
    static constexpr uint8_t GetSelectionOwner = 23;
    static constexpr uint8_t SendEvent = 25;
    static constexpr uint8_t QueryPointer = 38;
    static constexpr uint8_t CreatePixmap = 53;
    static constexpr uint8_t FreePixmap = 54;
    static constexpr uint8_t CopyArea = 62;
    static constexpr uint8_t PolyFillRectangle = 70;
    static constexpr uint8_t PutImage = 72;
    static constexpr uint8_t GetImage = 73;
    static constexpr uint8_t QueryExtension = 98;
    static constexpr uint8_t ListExtensions = 99;
    // Extension major opcodes — assigned out of the 128..255 range. Wine's
    // winex11.drv probes BIG-REQUESTS first thing; if absent, it falls
    // into a probe loop that never reaches CreateWindow against our
    // server. Keep these in sync with the QueryExtension handler.
    static constexpr uint8_t kGLXMajorOpcode = 128;
    static constexpr uint8_t kBigReqMajorOpcode = 130;
    static constexpr uint8_t kShapeMajorOpcode = 131;
    static constexpr uint8_t kXTestMajorOpcode = 132;
    static constexpr uint8_t kGEMajorOpcode = 133;  // X Generic Event
} // namespace X11Op

// X11 event types
namespace X11Event {
    static constexpr uint8_t KeyPress = 2;
    static constexpr uint8_t KeyRelease = 3;
    static constexpr uint8_t ButtonPress = 4;
    static constexpr uint8_t ButtonRelease = 5;
    static constexpr uint8_t MotionNotify = 6;
    static constexpr uint8_t Expose = 12;
    static constexpr uint8_t ConfigureNotify = 22;
    static constexpr uint8_t DestroyNotify = 17;
} // namespace X11Event

inline const char* x11OpcodeName(uint8_t op) {
    switch (op) {
        case X11Op::CreateWindow: return "CreateWindow";
        case X11Op::ChangeWindowAttributes: return "ChangeWindowAttributes";
        case X11Op::GetWindowAttributes: return "GetWindowAttributes";
        case X11Op::DestroyWindow: return "DestroyWindow";
        case 5: return "DestroySubwindows";
        case 7: return "ReparentWindow";
        case X11Op::MapWindow: return "MapWindow";
        case 9: return "MapSubwindows";
        case X11Op::UnmapWindow: return "UnmapWindow";
        case X11Op::ConfigureWindow: return "ConfigureWindow";
        case X11Op::GetGeometry: return "GetGeometry";
        case 15: return "QueryTree";
        case X11Op::InternAtom: return "InternAtom";
        case X11Op::GetAtomName: return "GetAtomName";
        case X11Op::ChangeProperty: return "ChangeProperty";
        case X11Op::DeleteProperty: return "DeleteProperty";
        case X11Op::GetProperty: return "GetProperty";
        case 21: return "ListProperties";
        case 22: return "SetSelectionOwner";
        case X11Op::GetSelectionOwner: return "GetSelectionOwner";
        case 24: return "ConvertSelection";
        case X11Op::SendEvent: return "SendEvent";
        case 26: return "GrabPointer";
        case 31: return "GrabKeyboard";
        case X11Op::QueryPointer: return "QueryPointer";
        case 42: return "SetInputFocus";
        case 43: return "GetInputFocus";
        case 55: return "CreateGC";
        case 56: return "ChangeGC";
        case 60: return "FreeGC";
        case 62: return "CopyArea";
        case X11Op::PolyFillRectangle: return "PolyFillRectangle";
        case X11Op::PutImage: return "PutImage";
        case 73: return "GetImage";
        case 78: return "CreateColormap";
        case 84: return "AllocColor";
        case X11Op::QueryExtension: return "QueryExtension";
        case X11Op::ListExtensions: return "ListExtensions";
        case X11Op::kGLXMajorOpcode: return "GLX";
        default: return "?";
    }
}

} // namespace guitarrackcraft
