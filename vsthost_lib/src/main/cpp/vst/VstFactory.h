#ifndef VSTHOST_VST_FACTORY_H
#define VSTHOST_VST_FACTORY_H

#include "../../../../../app/src/main/cpp/plugin/IPluginFactory.h"
#include <string>
#include <vector>
#include <mutex>

namespace guitarrackcraft {
class PluginRegistry;
}

namespace vsthost {

struct RegistryEntry {
    std::string uuid;
    std::string displayName;
    std::string format;     // "VST2" or "VST3"
    std::string dllPath;    // absolute path to the imported plugin DLL
    bool is64Bit = true;
};

/**
 * Factory for imported user VST2/VST3 plugins. Reads
 * <filesDir>/vst_plugins/registry.json which is written by the Manage VST
 * UI (Phase D). Each entry becomes one plugin in GuitarRackCraft's browser
 * grouped under author "Varcain" via the runtime metadata overlay.
 */
class VstFactory : public guitarrackcraft::IPluginFactory {
public:
    /**
     * @param filesDir      app filesDir (registry.json + wineprefix_p*)
     * @param wineRoot      extracted wine root (filesDir/wine/...)
     * @param assetsDir     extracted assets dir containing vst_host.exe
     * @param nativeLibDir  applicationInfo.nativeLibraryDir (for libwine_*.so resolution)
     */
    VstFactory(std::string filesDir,
               std::string wineRoot,
               std::string assetsDir,
               std::string nativeLibDir);
    ~VstFactory() override;

    std::string getFormat() const override { return "VST2"; }
    bool initialize() override;
    std::vector<guitarrackcraft::PluginInfo> enumeratePlugins() override;
    std::unique_ptr<guitarrackcraft::IPlugin> createPlugin(const std::string& pluginId) override;

    /** Re-read registry.json. Called after Manage VST import/remove. */
    void refresh();

private:
    bool loadRegistry();

    std::string filesDir_;
    std::string wineRoot_;
    std::string assetsDir_;
    std::string nativeLibDir_;

    std::mutex mu_;
    std::vector<RegistryEntry> entries_;     // guarded by mu_
    int nextDisplayNumber_ = 1;              // monotonic, hands out to WineVstPlugin instances
};

/**
 * Construct a VstFactory. :app's MainActivity calls this under
 * BuildConfig.HAS_VST_HOST and then passes the result to
 * PluginRegistry::registerFactory(std::move(factory)).
 *
 * Returning the factory (rather than registering it here) keeps the
 * :app ↔ :vsthost_lib link boundary clean — vsthost_lib doesn't need to
 * link against :app's PluginRegistry implementation.
 *
 * Marked visibility=default because vsthost_lib's CMake sets
 * -fvisibility=hidden globally; this is the one symbol :app needs to dlsym
 * (well, link) from libvsthost.so.
 */
__attribute__((visibility("default")))
std::unique_ptr<guitarrackcraft::IPluginFactory> createVstFactory(
    const std::string& filesDir,
    const std::string& wineRoot,
    const std::string& assetsDir,
    const std::string& nativeLibDir);

} // namespace vsthost

#endif
