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
}

} // namespace guitarrackcraft
