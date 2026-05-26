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

#include "X11PixmapStore.h"

namespace guitarrackcraft {

void X11PixmapStore::create(uint32_t pid, int w, int h, uint32_t fillColor) {
    PixmapData pm;
    pm.w = w;
    pm.h = h;
    pm.pixels.assign((size_t)w * h, fillColor);
    pixmaps_[pid] = std::move(pm);
}

void X11PixmapStore::destroy(uint32_t pid) {
    pixmaps_.erase(pid);
}

PixmapData* X11PixmapStore::get(uint32_t pid) {
    auto it = pixmaps_.find(pid);
    return (it != pixmaps_.end()) ? &it->second : nullptr;
}

const PixmapData* X11PixmapStore::get(uint32_t pid) const {
    auto it = pixmaps_.find(pid);
    return (it != pixmaps_.end()) ? &it->second : nullptr;
}

bool X11PixmapStore::exists(uint32_t pid) const {
    return pixmaps_.count(pid) > 0;
}

void X11PixmapStore::clear() {
    pixmaps_.clear();
}

} // namespace guitarrackcraft
