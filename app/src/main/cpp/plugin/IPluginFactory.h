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

#ifndef GUITARRACKCRAFT_IPLUGIN_FACTORY_H
#define GUITARRACKCRAFT_IPLUGIN_FACTORY_H

#include <memory>
#include <vector>
#include "IPlugin.h"

namespace guitarrackcraft {

/**
 * Abstract factory interface for creating plugin instances.
 * Each plugin format (LV2, CLAP, VST3) has its own factory implementation.
 */
class IPluginFactory {
public:
    virtual ~IPluginFactory() = default;

    /**
     * Get the plugin format identifier (e.g., "LV2", "CLAP", "VST3").
     */
    virtual std::string getFormat() const = 0;

    /**
     * Enumerate all available plugins from this factory.
     * @return Vector of plugin metadata
     */
    virtual std::vector<PluginInfo> enumeratePlugins() = 0;

    /**
     * Create a plugin instance by ID.
     * @param pluginId Unique plugin identifier
     * @return Plugin instance, or nullptr if plugin not found
     */
    virtual std::unique_ptr<IPlugin> createPlugin(const std::string& pluginId) = 0;

    /**
     * Initialize the factory (scan for plugins, etc.).
     * Must be called before enumeratePlugins() or createPlugin().
     */
    virtual bool initialize() = 0;
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_IPLUGIN_FACTORY_H
