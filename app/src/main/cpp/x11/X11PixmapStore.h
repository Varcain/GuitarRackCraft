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

struct PixmapData {
    int w = 0, h = 0;
    std::vector<uint32_t> pixels;
};

class X11PixmapStore {
public:
    void create(uint32_t pid, int w, int h, uint32_t fillColor = 0xFF302020);
    void destroy(uint32_t pid);
    PixmapData* get(uint32_t pid);
    const PixmapData* get(uint32_t pid) const;
    bool exists(uint32_t pid) const;
    void clear();

private:
    std::unordered_map<uint32_t, PixmapData> pixmaps_;
};

} // namespace guitarrackcraft
