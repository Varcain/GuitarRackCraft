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

#ifndef GUITARRACKCRAFT_LV2_PLUGIN_FACTORY_H
#define GUITARRACKCRAFT_LV2_PLUGIN_FACTORY_H

#include "../IPluginFactory.h"
#include <string>
#include <vector>
#include <memory>

#if defined(HAVE_LV2) && HAVE_LV2 == 1
#include <lilv/lilv.h>
#else
struct LilvWorld_;
#endif

namespace guitarrackcraft {

/**
 * Factory for creating LV2 plugins.
 * Scans for LV2 plugins in assets/lv2/ directory.
 */
class LV2PluginFactory : public IPluginFactory {
public:
    /** @param lv2Path Optional path to scan for LV2 bundles (e.g. extracted assets). If empty, uses default paths.
     *  @param nativeLibDir Path to the app's native library directory (for symlink-based plugin loading on API 29+).
     *  @param filesDir Path to the app's files directory (for state:mapPath).
     *  @param pluginLibDir Path to extracted PAD plugin .so (playstore flavor). Empty for full flavor. */
    explicit LV2PluginFactory(const std::string& lv2Path = std::string(),
                              const std::string& nativeLibDir = std::string(),
                              const std::string& filesDir = std::string(),
                              const std::string& pluginLibDir = std::string());
    ~LV2PluginFactory();

    std::string getFormat() const override { return "LV2"; }
    std::vector<PluginInfo> enumeratePlugins() override;
    std::unique_ptr<IPlugin> createPlugin(const std::string& pluginId) override;
    bool initialize() override;

private:
#if defined(HAVE_LV2) && HAVE_LV2 == 1
    LilvWorld* world_;
    void scanPlugins(const std::string& bundlePath);
    void rewriteManifestPaths(const std::string& bundlePath);
    int rewriteGuiextBinaryPaths(const std::string& bundleDir);
    /** If modgui.ttl exists for this plugin, set info.modguiBasePath and info.modguiIconTemplate. */
    void discoverModgui(const LilvPlugin* plugin, PluginInfo& info);
    /** If the TTL declares a guiext:X11UI, fill info.hasX11Ui / x11UiBinaryPath / x11UiUri. */
    void discoverX11UI(const LilvPlugin* plugin, PluginInfo& info);
#else
    LilvWorld_* world_;
    void scanPlugins(const std::string& bundlePath);
    void rewriteManifestPaths(const std::string& bundlePath);
#endif
    std::string lv2Path_;
    std::string nativeLibDir_;
    std::string filesDir_;
    std::string pluginLibDir_;
    std::vector<PluginInfo> plugins_;
    bool initialized_;
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_LV2_PLUGIN_FACTORY_H
