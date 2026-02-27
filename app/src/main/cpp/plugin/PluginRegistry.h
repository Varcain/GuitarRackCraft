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

#ifndef GUITARRACKCRAFT_PLUGIN_REGISTRY_H
#define GUITARRACKCRAFT_PLUGIN_REGISTRY_H

#include <memory>
#include <vector>
#include <unordered_map>
#include "IPluginFactory.h"
#include "IPlugin.h"

namespace guitarrackcraft {

/**
 * Central registry for all plugin factories.
 * Provides unified access to plugins across all formats.
 */
class PluginRegistry {
public:
    PluginRegistry() = default;
    ~PluginRegistry() = default;

    /**
     * Register a plugin factory.
     */
    void registerFactory(std::unique_ptr<IPluginFactory> factory);

    /**
     * Initialize all registered factories.
     */
    bool initializeAll();

    /**
     * Get all available plugins from all factories.
     */
    std::vector<PluginInfo> getAllPlugins() const;

    /**
     * Create a plugin instance by ID.
     * @param pluginId Unique plugin identifier (format: "format:plugin_id")
     * @return Plugin instance, or nullptr if not found
     */
    std::unique_ptr<IPlugin> createPlugin(const std::string& pluginId) const;

    /**
     * Get plugin info by ID.
     */
    PluginInfo getPluginInfo(const std::string& pluginId) const;

private:
    std::vector<std::unique_ptr<IPluginFactory>> factories_;
    std::unordered_map<std::string, PluginInfo> pluginCache_;
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_PLUGIN_REGISTRY_H
