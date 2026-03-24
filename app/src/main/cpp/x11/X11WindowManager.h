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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace guitarrackcraft {

struct WindowPos {
    int x = 0, y = 0;
    uint32_t parent = 0;
};

struct HitResult {
    uint32_t wid;
    int localX;
    int localY;
};

class X11WindowManager {
public:
    explicit X11WindowManager(uint32_t rootWindowId);

    void createWindow(uint32_t wid, uint32_t parent, int x, int y, int w, int h);
    void destroyWindow(uint32_t wid);
    void mapWindow(uint32_t wid);
    void unmapWindow(uint32_t wid);
    void configureWindow(uint32_t wid, int x, int y, int w, int h);
    void setEventMask(uint32_t wid, uint32_t mask);
    bool setPositionX(uint32_t wid, int x);
    bool setPositionY(uint32_t wid, int y);
    void setSize(uint32_t wid, int w, int h);

    // Raise an entire subtree rooted at wid to the front of childWindows_.
    // Returns the number of windows moved (for logging).
    size_t raiseSubtreeToFront(uint32_t wid);

    // Return absolute clip rects {x1,y1,x2,y2} for mapped children of the given parent.
    struct Rect { int x1, y1, x2, y2; };
    std::vector<Rect> getMappedChildRectsOf(uint32_t parent) const;

    std::pair<int, int> getAbsolutePos(uint32_t wid) const;
    HitResult hitTest(int x, int y) const;

    bool exists(uint32_t wid) const;
    bool isUnmapped(uint32_t wid) const;
    bool isMapped(uint32_t wid) const;
    std::pair<int, int> getSize(uint32_t wid) const;
    WindowPos getPosition(uint32_t wid) const;
    uint32_t getEventMask(uint32_t wid) const;
    const std::vector<uint32_t>& childWindows() const;

    int originalChildW() const { return originalChildW_; }
    int originalChildH() const { return originalChildH_; }

    void clear();

private:
    uint32_t rootWindowId_;
    std::vector<uint32_t> childWindows_;
    std::unordered_map<uint32_t, std::pair<int, int>> windowSizes_;
    std::unordered_map<uint32_t, WindowPos> windowPositions_;
    std::unordered_set<uint32_t> unmappedWindows_;
    std::unordered_map<uint32_t, uint32_t> windowEventMasks_;
    int originalChildW_ = 0, originalChildH_ = 0;
};

} // namespace guitarrackcraft
