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
#include <mutex>
#include <string>
#include <unordered_map>

namespace guitarrackcraft {

class X11AtomStore {
public:
    X11AtomStore();
    // InternAtom: look up or create. Returns 0 (None) if onlyIfExists and not found.
    uint32_t intern(const std::string& name, bool onlyIfExists);

    // GetAtomName: reverse lookup. Returns empty string if not found.
    std::string getName(uint32_t atomId) const;

    // Forget interned atoms; only the predefined ones remain.
    void clear();

private:
    // Every X11 client connection runs on its own thread and interns /
    // looks up atoms concurrently, so all access is serialized.
    mutable std::mutex mutex_;
    std::unordered_map<std::string, uint32_t> nameToId_;
    std::unordered_map<uint32_t, std::string> idToName_;
    uint32_t nextId_ = 1;
};

} // namespace guitarrackcraft
