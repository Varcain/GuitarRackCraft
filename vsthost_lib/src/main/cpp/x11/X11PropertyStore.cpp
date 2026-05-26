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

#include "X11PropertyStore.h"
#include <algorithm>

namespace guitarrackcraft {

void X11PropertyStore::change(uint32_t window, uint32_t property, uint32_t type,
                               uint8_t format, uint8_t mode, const uint8_t* data,
                               uint32_t numElements) {
    if (format != 8 && format != 16 && format != 32) return;
    size_t elementBytes = format / 8;
    size_t dataBytes = (size_t)numElements * elementBytes;

    auto& windowProps = store_[window];
    auto& prop = windowProps[property];

    if (mode == ModeReplace || prop.format == 0) {
        prop.type = type;
        prop.format = format;
        prop.data.assign(data, data + dataBytes);
    } else if (mode == ModePrepend && prop.format == format) {
        std::vector<uint8_t> newData(data, data + dataBytes);
        newData.insert(newData.end(), prop.data.begin(), prop.data.end());
        prop.data = std::move(newData);
        prop.type = type;
    } else if (mode == ModeAppend && prop.format == format) {
        prop.data.insert(prop.data.end(), data, data + dataBytes);
        prop.type = type;
    }
}

X11PropertyStore::Property X11PropertyStore::get(uint32_t window, uint32_t property,
                                                  uint32_t reqType, uint32_t offset,
                                                  uint32_t maxLen, uint32_t& bytesAfter) const {
    bytesAfter = 0;
    auto winIt = store_.find(window);
    if (winIt == store_.end()) return {};

    auto propIt = winIt->second.find(property);
    if (propIt == winIt->second.end()) return {};

    const auto& prop = propIt->second;

    // If reqType is specified and doesn't match, return type info but no data
    if (reqType != 0 && reqType != prop.type) {
        Property result;
        result.type = prop.type;
        result.format = 0;  // format=0 signals type mismatch
        bytesAfter = (uint32_t)prop.data.size();
        return result;
    }

    // offset and maxLen are in 4-byte units
    size_t byteOffset = (size_t)offset * 4;
    size_t maxBytes = (maxLen == 0) ? 0 : (size_t)maxLen * 4;

    if (byteOffset >= prop.data.size()) {
        bytesAfter = 0;
        Property result;
        result.type = prop.type;
        result.format = prop.format;
        return result;
    }

    size_t available = prop.data.size() - byteOffset;
    size_t toReturn = (maxLen == 0) ? 0 : std::min(available, maxBytes);
    bytesAfter = (uint32_t)(available - toReturn);

    Property result;
    result.type = prop.type;
    result.format = prop.format;
    if (toReturn > 0) {
        result.data.assign(prop.data.begin() + byteOffset,
                           prop.data.begin() + byteOffset + toReturn);
    }
    return result;
}

void X11PropertyStore::remove(uint32_t window, uint32_t property) {
    auto winIt = store_.find(window);
    if (winIt != store_.end()) {
        winIt->second.erase(property);
        if (winIt->second.empty()) store_.erase(winIt);
    }
}

std::vector<uint32_t> X11PropertyStore::list(uint32_t window) const {
    std::vector<uint32_t> atoms;
    auto winIt = store_.find(window);
    if (winIt != store_.end()) {
        for (auto& [atom, _] : winIt->second) {
            atoms.push_back(atom);
        }
        std::sort(atoms.begin(), atoms.end());
    }
    return atoms;
}

void X11PropertyStore::clearWindow(uint32_t window) {
    store_.erase(window);
}

void X11PropertyStore::clear() {
    store_.clear();
}

} // namespace guitarrackcraft
