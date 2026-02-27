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

#include <mutex>
#include <unordered_map>

namespace guitarrackcraft {

struct DisplayState {
    enum class Phase {
        None,
        Attached,
        Creating,
        Ready,
        Destroying
    };

    Phase phase = Phase::None;
    int pluginIndex = -1;
    bool detachPending = false;
};

// Global display state map and mutex, shared between NativeBridge and PluginUIGuard.
inline std::mutex& displayStateMutex() {
    static std::mutex m;
    return m;
}

inline std::unordered_map<int, DisplayState>& displayStates() {
    static std::unordered_map<int, DisplayState> s;
    return s;
}

} // namespace guitarrackcraft
