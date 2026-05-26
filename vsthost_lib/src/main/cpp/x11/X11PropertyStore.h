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
#include <vector>

namespace guitarrackcraft {

class X11PropertyStore {
public:
    struct Property {
        uint32_t type = 0;     // atom identifying the property type (e.g., STRING, CARDINAL)
        uint8_t format = 0;    // 8, 16, or 32 (element size in bits)
        std::vector<uint8_t> data;
    };

    // X11 ChangeProperty modes
    static constexpr uint8_t ModeReplace = 0;
    static constexpr uint8_t ModePrepend = 1;
    static constexpr uint8_t ModeAppend = 2;

    // ChangeProperty: set/prepend/append property data on a window.
    // numElements is the count of format-sized elements (not bytes).
    void change(uint32_t window, uint32_t property, uint32_t type,
                uint8_t format, uint8_t mode, const uint8_t* data, uint32_t numElements);

    // GetProperty: retrieve property data.
    // reqType=0 means any type. offset and maxLen are in 4-byte units.
    // Returns the property if found (empty Property with format=0 if not found).
    // bytesAfter is set to the remaining bytes after the returned portion.
    Property get(uint32_t window, uint32_t property, uint32_t reqType,
                 uint32_t offset, uint32_t maxLen, uint32_t& bytesAfter) const;

    // DeleteProperty: remove a specific property from a window.
    void remove(uint32_t window, uint32_t property);

    // ListProperties: return all property atoms set on a window.
    std::vector<uint32_t> list(uint32_t window) const;

    // Clear all properties for a specific window.
    void clearWindow(uint32_t window);

    // Clear everything.
    void clear();

private:
    // window -> (property atom -> Property)
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, Property>> store_;
};

} // namespace guitarrackcraft
