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

#include "X11WindowManager.h"
#include <algorithm>
#include <unordered_set>

namespace guitarrackcraft {

X11WindowManager::X11WindowManager(uint32_t rootWindowId)
    : rootWindowId_(rootWindowId) {}

void X11WindowManager::createWindow(uint32_t wid, uint32_t parent, int x, int y, int w, int h) {
    childWindows_.push_back(wid);
    windowSizes_[wid] = {w, h};
    windowPositions_[wid] = {x, y, parent};
    unmappedWindows_.insert(wid); // New windows start unmapped

    // Track the first child window's original size
    if (childWindows_.size() == 1) {
        originalChildW_ = w;
        originalChildH_ = h;
    }
}

void X11WindowManager::destroyWindow(uint32_t wid) {
    childWindows_.erase(
        std::remove(childWindows_.begin(), childWindows_.end(), wid),
        childWindows_.end());
    windowSizes_.erase(wid);
    windowPositions_.erase(wid);
    unmappedWindows_.erase(wid);
    windowEventMasks_.erase(wid);
}

void X11WindowManager::mapWindow(uint32_t wid) {
    unmappedWindows_.erase(wid);
}

void X11WindowManager::unmapWindow(uint32_t wid) {
    unmappedWindows_.insert(wid);
}

void X11WindowManager::configureWindow(uint32_t wid, int x, int y, int w, int h) {
    if (w > 0 && h > 0) {
        windowSizes_[wid] = {w, h};
    }
    auto posIt = windowPositions_.find(wid);
    if (posIt != windowPositions_.end()) {
        posIt->second.x = x;
        posIt->second.y = y;
    }
}

void X11WindowManager::setEventMask(uint32_t wid, uint32_t mask) {
    windowEventMasks_[wid] = mask;
}

std::pair<int, int> X11WindowManager::getAbsolutePos(uint32_t wid) const {
    int ax = 0, ay = 0;
    uint32_t cur = wid;
    for (int depth = 0; depth < 32; depth++) {
        auto it = windowPositions_.find(cur);
        if (it == windowPositions_.end()) break;
        ax += it->second.x;
        ay += it->second.y;
        cur = it->second.parent;
        if (!childWindows_.empty() && cur == childWindows_[0]) break;
        if (cur == rootWindowId_ || cur == 0) break;
    }
    return {ax, ay};
}

HitResult X11WindowManager::hitTest(int x, int y) const {
    uint32_t topWin = childWindows_.empty() ? rootWindowId_ : childWindows_[0];
    HitResult best = {topWin, x, y};

    for (int i = (int)childWindows_.size() - 1; i >= 1; i--) {
        uint32_t wid = childWindows_[i];
        if (unmappedWindows_.count(wid)) continue;
        auto posIt = windowPositions_.find(wid);
        auto sizeIt = windowSizes_.find(wid);
        if (posIt == windowPositions_.end() || sizeIt == windowSizes_.end()) continue;

        auto absPos = getAbsolutePos(wid);
        int wx = absPos.first, wy = absPos.second;
        int ww = sizeIt->second.first, wh = sizeIt->second.second;

        if (x >= wx && x < wx + ww && y >= wy && y < wy + wh) {
            best = {wid, x - wx, y - wy};
            break;
        }
    }
    return best;
}

bool X11WindowManager::exists(uint32_t wid) const {
    return windowSizes_.count(wid) > 0;
}

bool X11WindowManager::isUnmapped(uint32_t wid) const {
    return unmappedWindows_.count(wid) > 0;
}

bool X11WindowManager::isMapped(uint32_t wid) const {
    return exists(wid) && !isUnmapped(wid);
}

std::pair<int, int> X11WindowManager::getSize(uint32_t wid) const {
    auto it = windowSizes_.find(wid);
    if (it != windowSizes_.end()) return it->second;
    return {0, 0};
}

WindowPos X11WindowManager::getPosition(uint32_t wid) const {
    auto it = windowPositions_.find(wid);
    if (it != windowPositions_.end()) return it->second;
    return {};
}

uint32_t X11WindowManager::getEventMask(uint32_t wid) const {
    auto it = windowEventMasks_.find(wid);
    if (it != windowEventMasks_.end()) return it->second;
    return 0;
}

const std::vector<uint32_t>& X11WindowManager::childWindows() const {
    return childWindows_;
}

bool X11WindowManager::setPositionX(uint32_t wid, int x) {
    auto it = windowPositions_.find(wid);
    if (it != windowPositions_.end() && it->second.x != x) {
        it->second.x = x;
        return true;
    }
    return false;
}

bool X11WindowManager::setPositionY(uint32_t wid, int y) {
    auto it = windowPositions_.find(wid);
    if (it != windowPositions_.end() && it->second.y != y) {
        it->second.y = y;
        return true;
    }
    return false;
}

void X11WindowManager::setSize(uint32_t wid, int w, int h) {
    windowSizes_[wid] = {w, h};
}

size_t X11WindowManager::raiseSubtreeToFront(uint32_t wid) {
    if (childWindows_.empty() || wid == childWindows_[0]) return 0;
    auto posIt = windowPositions_.find(wid);
    if (posIt == windowPositions_.end() || posIt->second.parent != rootWindowId_)
        return 0;

    // BFS to collect entire subtree
    std::unordered_set<uint32_t> subtree;
    subtree.insert(wid);
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [cw, pos] : windowPositions_) {
            if (subtree.count(pos.parent) && !subtree.count(cw)) {
                subtree.insert(cw);
                changed = true;
            }
        }
    }
    // Sort by window ID (= creation order)
    std::vector<uint32_t> toMove(subtree.begin(), subtree.end());
    std::sort(toMove.begin(), toMove.end());
    // Remove subtree from childWindows
    childWindows_.erase(
        std::remove_if(childWindows_.begin(), childWindows_.end(),
            [&subtree](uint32_t w) { return subtree.count(w); }),
        childWindows_.end());
    // Add back at the end in creation order
    for (auto w : toMove)
        childWindows_.push_back(w);
    return toMove.size();
}

std::vector<X11WindowManager::Rect> X11WindowManager::getMappedChildRectsOf(uint32_t parent) const {
    std::vector<Rect> rects;
    for (auto& [cwid, cpos] : windowPositions_) {
        if (cpos.parent == parent && !unmappedWindows_.count(cwid)) {
            auto sizeIt = windowSizes_.find(cwid);
            if (sizeIt != windowSizes_.end()) {
                auto absPos = getAbsolutePos(cwid);
                rects.push_back({
                    absPos.first, absPos.second,
                    absPos.first + sizeIt->second.first,
                    absPos.second + sizeIt->second.second
                });
            }
        }
    }
    return rects;
}

std::vector<X11WindowManager::Rect> X11WindowManager::getMappedSiblingRectsAbove(uint32_t wid) const {
    std::vector<Rect> rects;

    // Find wid's parent
    auto posIt = windowPositions_.find(wid);
    if (posIt == windowPositions_.end()) return rects;
    uint32_t parent = posIt->second.parent;

    // Find wid's index in childWindows_
    bool found = false;
    size_t widIndex = 0;
    for (size_t i = 0; i < childWindows_.size(); i++) {
        if (childWindows_[i] == wid) {
            widIndex = i;
            found = true;
            break;
        }
    }
    if (!found) return rects;

    // Collect rects for mapped siblings above wid (higher stacking = later index)
    for (size_t i = widIndex + 1; i < childWindows_.size(); i++) {
        uint32_t sibWid = childWindows_[i];
        if (unmappedWindows_.count(sibWid)) continue;

        auto sibPosIt = windowPositions_.find(sibWid);
        if (sibPosIt == windowPositions_.end()) continue;
        if (sibPosIt->second.parent != parent) continue;

        auto sizeIt = windowSizes_.find(sibWid);
        if (sizeIt == windowSizes_.end()) continue;

        auto absPos = getAbsolutePos(sibWid);
        rects.push_back({
            absPos.first, absPos.second,
            absPos.first + sizeIt->second.first,
            absPos.second + sizeIt->second.second
        });
    }
    return rects;
}

void X11WindowManager::clear() {
    childWindows_.clear();
    windowSizes_.clear();
    windowPositions_.clear();
    unmappedWindows_.clear();
    windowEventMasks_.clear();
    originalChildW_ = 0;
    originalChildH_ = 0;
}

} // namespace guitarrackcraft
