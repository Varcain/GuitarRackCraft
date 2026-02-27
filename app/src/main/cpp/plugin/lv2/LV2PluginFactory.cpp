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

#include "LV2PluginFactory.h"
#include "LV2Plugin.h"
#include "LV2Utils.h"
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_TAG "LV2PluginFactory"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#if defined(HAVE_LV2) && HAVE_LV2 == 1
#include <lilv/lilv.h>
#endif

namespace guitarrackcraft {

#if defined(HAVE_LV2) && HAVE_LV2 == 1

LV2PluginFactory::LV2PluginFactory(const std::string& lv2Path, const std::string& nativeLibDir,
                                   const std::string& filesDir, const std::string& pluginLibDir)
    : world_(nullptr)
    , lv2Path_(lv2Path)
    , nativeLibDir_(nativeLibDir)
    , filesDir_(filesDir)
    , pluginLibDir_(pluginLibDir)
    , initialized_(false)
{
}

LV2PluginFactory::~LV2PluginFactory() {
    if (world_) {
        lilv_world_free(world_);
    }
}

bool LV2PluginFactory::initialize() {
    if (initialized_) {
        return true;
    }

    world_ = lilv_world_new();
    if (!world_) {
        LOGE("Failed to create LV2 world");
        return false;
    }
    
    // Scan path set by nativeSetLv2Path() (extracted assets), then fallback paths
    LOGI("LV2 scan path: '%s'", lv2Path_.c_str());
    if (!lv2Path_.empty()) {
        // Create symlinks from bundle dirs to native lib dir before scanning
        if (!nativeLibDir_.empty()) {
            rewriteManifestPaths(lv2Path_);
        }
        scanPlugins(lv2Path_);
    }
    const char* fallbackPaths[] = {
        "/data/data/com.varcain.guitarrackcraft/files/lv2",
        "/sdcard/Android/data/com.varcain.guitarrackcraft/files/lv2",
        nullptr
    };
    for (int i = 0; fallbackPaths[i]; ++i) {
        if (lv2Path_ != fallbackPaths[i]) {
            scanPlugins(fallbackPaths[i]);
        }
    }
    
    // Parse specifications and plugin classes so get_all_plugins() returns discovered plugins
    lilv_world_load_specifications(world_);
    lilv_world_load_plugin_classes(world_);
    
    // Enumerate all discovered plugins
    const LilvPlugins* plugins = lilv_world_get_all_plugins(world_);
    int x11Count = 0, modguiCount = 0;
    LILV_FOREACH(plugins, i, plugins) {
        const LilvPlugin* plugin = lilv_plugins_get(plugins, i);
        PluginInfo info;

        const LilvNode* uri = lilv_plugin_get_uri(plugin);
        const LilvNode* name = lilv_plugin_get_name(plugin);

        if (uri) {
            info.id = lilv_node_as_string(uri);
        }
        if (name) {
            info.name = lilv_node_as_string(name);
        }
        info.format = "LV2";

        // Get port count for info
        uint32_t numPorts = lilv_plugin_get_num_ports(plugin);
        info.ports.reserve(numPorts);

        // Discover modgui (modgui.ttl + iconTemplate)
        discoverModgui(plugin, info);

        // Discover X11UI (guiext:X11UI in the TTL). Always prefer X11 over modgui when both present.
        discoverX11UI(plugin, info);

        if (info.hasX11Ui) x11Count++;
        if (!info.modguiBasePath.empty()) modguiCount++;

        plugins_.push_back(info);
    }

    initialized_ = true;
    LOGI("LV2 plugin factory initialized: %zu plugins found (x11=%d, modgui=%d)",
         plugins_.size(), x11Count, modguiCount);
    return true;
}

std::vector<PluginInfo> LV2PluginFactory::enumeratePlugins() {
    if (!initialized_) {
        return {};
    }
    return plugins_;
}

std::unique_ptr<IPlugin> LV2PluginFactory::createPlugin(const std::string& pluginId) {
    if (!initialized_ || !world_) {
        return nullptr;
    }

    LilvNode* uri = lilv_new_uri(world_, pluginId.c_str());
    if (!uri) {
        LOGE("Invalid plugin URI: %s", pluginId.c_str());
        return nullptr;
    }
    
    const LilvPlugins* plugins = lilv_world_get_all_plugins(world_);
    const LilvPlugin* plugin = lilv_plugins_get_by_uri(plugins, uri);
    lilv_node_free(uri);
    
    if (!plugin) {
        LOGE("LV2 plugin not found: %s", pluginId.c_str());
        return nullptr;
    }
    
    static constexpr float kDefaultSampleRate = 48000.0f;
    auto lv2Plugin = std::make_unique<LV2Plugin>(plugin, world_, kDefaultSampleRate, filesDir_);
    if (!lv2Plugin->hasInstance()) {
        LOGE("LV2 plugin could not be instantiated (binary missing or load failed): %s", pluginId.c_str());
        return nullptr;
    }
    return lv2Plugin;
}

void LV2PluginFactory::discoverModgui(const LilvPlugin* plugin, PluginInfo& info) {
    discoverModguiMetadata(plugin, info);
}

void LV2PluginFactory::discoverX11UI(const LilvPlugin* plugin, PluginInfo& info) {
    LilvNode* x11UiClass = lilv_new_uri(world_, "http://lv2plug.in/ns/extensions/ui#X11UI");
    if (!x11UiClass) return;

    LilvUIs* uis = lilv_plugin_get_uis(plugin);
    if (!uis) {
        lilv_node_free(x11UiClass);
        return;
    }

    LILV_FOREACH(uis, u, uis) {
        const LilvUI* ui = lilv_uis_get(uis, u);
        if (!lilv_ui_is_a(ui, x11UiClass)) continue;

        std::string binaryPath = resolveX11UIBinaryPath(ui, plugin, world_);
        if (binaryPath.empty()) continue;

        info.hasX11Ui = true;
        info.x11UiBinaryPath = binaryPath;
        info.x11UiUri = lilv_node_as_string(lilv_ui_get_uri(ui));
        break;
    }

    lilv_uis_free(uis);
    lilv_node_free(x11UiClass);
}

void LV2PluginFactory::scanPlugins(const std::string& bundlePath) {
    if (bundlePath.empty() || !world_) {
        return;
    }

    DIR* dir = opendir(bundlePath.c_str());
    if (!dir) {
        LOGE("scanPlugins: opendir failed path='%s' errno=%d (%s)", bundlePath.c_str(), errno, std::strerror(errno));
        return;
    }

    int bundleCount = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        std::string fullPath = bundlePath + "/" + entry->d_name;
        struct stat st;
        if (stat(fullPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            // Check if it's an LV2 bundle (has manifest.ttl)
            std::string manifestPath = fullPath + "/manifest.ttl";
            if (access(manifestPath.c_str(), F_OK) == 0) {
                // This is an LV2 bundle - load it (API requires URI with trailing slash)
                std::string pathWithSlash = fullPath;
                if (pathWithSlash.back() != '/') {
                    pathWithSlash += '/';
                }
                LilvNode* bundleUri = lilv_new_file_uri(world_, nullptr, pathWithSlash.c_str());
                if (bundleUri) {
                    lilv_world_load_bundle(world_, bundleUri);
                    bundleCount++;
                    lilv_node_free(bundleUri);
                }
            } else {
                // Not a bundle, recurse into subdirectories (e.g., GxPlugins.lv2/)
                scanPlugins(fullPath);
            }
        }
    }

    closedir(dir);
    if (bundleCount > 0) {
        LOGI("scanPlugins: loaded %d bundles from %s", bundleCount, bundlePath.c_str());
    }
}

// Returns number of UI binary paths rewritten in this bundle directory.
int LV2PluginFactory::rewriteGuiextBinaryPaths(const std::string& bundleDir) {
    DIR* dir = opendir(bundleDir.c_str());
    if (!dir) return 0;

    int rewriteCount = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string name = entry->d_name;
        if (name.size() < 4 || name.substr(name.size() - 4) != ".ttl") continue;

        std::string ttlPath = bundleDir + "/" + name;
        FILE* f = fopen(ttlPath.c_str(), "r");
        if (!f) continue;

        std::string content;
        char buf[1024];
        while (fgets(buf, sizeof(buf), f)) {
            content += buf;
        }
        fclose(f);

        // Find all UI binary references and rewrite them to nativeLibDir.
        // Handles both "guiext:binary" (xputty plugins) and "ui:binary" (DPF/AIDA-X).
        bool modified = false;
        std::string newContent = content;
        static const char* kBinaryKeywords[] = { "guiext:binary", "ui:binary", nullptr };
        for (const char** kw = kBinaryKeywords; *kw; ++kw) {
            size_t kwLen = strlen(*kw);
            size_t searchPos = 0;
            while (true) {
                size_t binaryPos = newContent.find(*kw, searchPos);
                if (binaryPos == std::string::npos) break;

                size_t lt = newContent.find('<', binaryPos);
                size_t gt = (lt != std::string::npos) ? newContent.find('>', lt) : std::string::npos;
                if (lt == std::string::npos || gt == std::string::npos) {
                    searchPos = binaryPos + kwLen;
                    continue;
                }

                std::string binaryUri = newContent.substr(lt + 1, gt - lt - 1);
                // Skip if already rewritten (has file:// prefix)
                if (binaryUri.find("file://") == 0) {
                    searchPos = gt + 1;
                    continue;
                }

                size_t lastSlash = binaryUri.rfind('/');
                std::string soName = (lastSlash != std::string::npos)
                    ? binaryUri.substr(lastSlash + 1) : binaryUri;

                if (soName.size() <= 3 || soName.substr(soName.size() - 3) != ".so") {
                    searchPos = gt + 1;
                    continue;
                }

                std::string nativeLibPath = nativeLibDir_ + "/lib" + soName;
                std::string pluginLibPath = pluginLibDir_.empty() ? std::string() : (pluginLibDir_ + "/lib" + soName);
                std::string resolvedPath;

                if (access(nativeLibPath.c_str(), F_OK) == 0) {
                    resolvedPath = nativeLibPath;
                } else if (!pluginLibPath.empty() && access(pluginLibPath.c_str(), F_OK) == 0) {
                    resolvedPath = pluginLibPath;
                } else {
                    searchPos = gt + 1;
                    continue;
                }

                std::string newToken = "<file://" + resolvedPath + ">";
                newContent.replace(lt, gt - lt + 1, newToken);
                modified = true;
                rewriteCount++;
                searchPos = lt + newToken.size();
            }
        }

        if (modified) {
            FILE* fw = fopen(ttlPath.c_str(), "w");
            if (fw) {
                fwrite(newContent.c_str(), 1, newContent.size(), fw);
                fclose(fw);
            } else {
                LOGE("Failed to rewrite TTL: %s: %s", ttlPath.c_str(), strerror(errno));
            }
        }
    }
    closedir(dir);
    return rewriteCount;
}

void LV2PluginFactory::rewriteManifestPaths(const std::string& basePath) {
    DIR* dir = opendir(basePath.c_str());
    if (!dir) return;

    int rewriteCount = 0;
    int uiRewriteCount = 0;
    int missingCount = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        std::string fullPath = basePath + "/" + entry->d_name;
        struct stat st;
        if (stat(fullPath.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        std::string manifestPath = fullPath + "/manifest.ttl";
        if (access(manifestPath.c_str(), F_OK) == 0) {
            // It's an LV2 bundle - rewrite manifest.ttl to point lv2:binary
            // to the native lib dir so dlopen uses an exec-permitted path
            FILE* f = fopen(manifestPath.c_str(), "r");
            if (!f) continue;

            // Read entire manifest
            std::string content;
            char buf[1024];
            while (fgets(buf, sizeof(buf), f)) {
                content += buf;
            }
            fclose(f);

            // Find lv2:binary <soname.so> and replace with absolute path
            size_t binaryPos = content.find("lv2:binary");
            if (binaryPos == std::string::npos) continue;

            size_t lt = content.find('<', binaryPos);
            size_t gt = content.find('>', lt != std::string::npos ? lt : 0);
            if (lt == std::string::npos || gt == std::string::npos) continue;

            std::string binaryUri = content.substr(lt + 1, gt - lt - 1);
            size_t lastSlash = binaryUri.rfind('/');
            std::string soName = (lastSlash != std::string::npos)
                ? binaryUri.substr(lastSlash + 1) : binaryUri;

            if (soName.size() <= 3 || soName.substr(soName.size() - 3) != ".so") continue;

            std::string nativeLibPath = nativeLibDir_ + "/lib" + soName;
            std::string pluginLibPath = pluginLibDir_.empty() ? std::string() : (pluginLibDir_ + "/lib" + soName);
            std::string bundleSoPath = fullPath + "/" + soName;

            // Resolve which path to use for lv2:binary.
            // Search order: nativeLibDir (full flavor) → pluginLibDir (PAD extraction) → bundle fallback
            std::string binaryUriValue;
            bool haveInLibDir = (access(nativeLibPath.c_str(), F_OK) == 0);
            bool haveInPluginDir = (!pluginLibPath.empty() && access(pluginLibPath.c_str(), F_OK) == 0);
            bool haveInBundle = (access(bundleSoPath.c_str(), F_OK) == 0);

            if (haveInLibDir) {
                binaryUriValue = "file://" + nativeLibPath;
            } else if (haveInPluginDir) {
                binaryUriValue = "file://" + pluginLibPath;
            } else if (haveInBundle) {
                binaryUriValue = "file://" + bundleSoPath;
            } else {
                missingCount++;
                continue;
            }

            // Replace ALL occurrences of <soname.so> (multi-plugin bundles share the same binary)
            std::string oldToken = "<" + binaryUri + ">";
            std::string newToken = "<" + binaryUriValue + ">";
            std::string newContent = content;
            size_t pos = 0;
            while ((pos = newContent.find(oldToken, pos)) != std::string::npos) {
                newContent.replace(pos, oldToken.size(), newToken);
                pos += newToken.size();
            }

            // Write back
            FILE* fw = fopen(manifestPath.c_str(), "w");
            if (fw) {
                fwrite(newContent.c_str(), 1, newContent.size(), fw);
                fclose(fw);
                rewriteCount++;
            } else {
                LOGE("Failed to rewrite manifest: %s: %s", manifestPath.c_str(), strerror(errno));
            }

            // Also rewrite guiext:binary in all .ttl files in this bundle
            uiRewriteCount += rewriteGuiextBinaryPaths(fullPath);
        } else {
            // Not a bundle, recurse (e.g. GxPlugins.lv2/)
            rewriteManifestPaths(fullPath);
        }
    }
    closedir(dir);
    if (rewriteCount > 0 || missingCount > 0) {
        LOGI("rewriteManifestPaths: %d manifests rewritten, %d UI binaries rewritten, %d missing (%s)",
             rewriteCount, uiRewriteCount, missingCount, basePath.c_str());
    }
}

#else // HAVE_LV2 not defined or == 0 - stub implementation

LV2PluginFactory::LV2PluginFactory(const std::string& /*lv2Path*/, const std::string& /*nativeLibDir*/,
                                   const std::string& /*filesDir*/, const std::string& /*pluginLibDir*/)
    : world_(nullptr)
    , initialized_(false)
{
    LOGE("LV2PluginFactory: LV2 libraries not available (stub mode)");
}

LV2PluginFactory::~LV2PluginFactory() {
}

bool LV2PluginFactory::initialize() {
    initialized_ = true;
    LOGI("LV2PluginFactory: Stub mode - no plugins available");
    return true;
}

std::vector<PluginInfo> LV2PluginFactory::enumeratePlugins() {
    return {};
}

std::unique_ptr<IPlugin> LV2PluginFactory::createPlugin(const std::string& pluginId) {
    LOGE("LV2PluginFactory: Cannot create plugin - LV2 libraries not available");
    return nullptr;
}

void LV2PluginFactory::scanPlugins(const std::string& bundlePath) {
    // Stub
}

void LV2PluginFactory::rewriteManifestPaths(const std::string& basePath) {
    // Stub
}

#endif // HAVE_LV2 == 1

} // namespace guitarrackcraft
