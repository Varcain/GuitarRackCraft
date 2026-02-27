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

#include "PluginRegistry.h"
#include <algorithm>

namespace guitarrackcraft {

void PluginRegistry::registerFactory(std::unique_ptr<IPluginFactory> factory) {
    if (factory) {
        factories_.push_back(std::move(factory));
    }
}

bool PluginRegistry::initializeAll() {
    pluginCache_.clear();
    bool allSucceeded = true;

    for (auto& factory : factories_) {
        if (!factory->initialize()) {
            allSucceeded = false;
            continue;
        }

        // Cache plugin info
        auto plugins = factory->enumeratePlugins();
        for (const auto& plugin : plugins) {
            std::string fullId = factory->getFormat() + ":" + plugin.id;
            pluginCache_[fullId] = plugin;
        }
    }

    return allSucceeded;
}

std::vector<PluginInfo> PluginRegistry::getAllPlugins() const {
    std::vector<PluginInfo> allPlugins;
    allPlugins.reserve(pluginCache_.size());

    for (const auto& pair : pluginCache_) {
        allPlugins.push_back(pair.second);
    }

    return allPlugins;
}

std::unique_ptr<IPlugin> PluginRegistry::createPlugin(const std::string& pluginId) const {
    // Parse format:plugin_id
    size_t colonPos = pluginId.find(':');
    if (colonPos == std::string::npos) {
        return nullptr;
    }

    std::string format = pluginId.substr(0, colonPos);
    std::string id = pluginId.substr(colonPos + 1);

    // Find factory for this format
    for (const auto& factory : factories_) {
        if (factory->getFormat() == format) {
            return factory->createPlugin(id);
        }
    }

    return nullptr;
}

PluginInfo PluginRegistry::getPluginInfo(const std::string& pluginId) const {
    auto it = pluginCache_.find(pluginId);
    if (it != pluginCache_.end()) {
        return it->second;
    }
    return PluginInfo{}; // Return empty info if not found
}

} // namespace guitarrackcraft
