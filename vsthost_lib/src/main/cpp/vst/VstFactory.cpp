#include "VstFactory.h"
#include "WineVstPlugin.h"
#include "../util/log.h"
#include <fstream>
#include <sstream>
#include <utility>

namespace vsthost {

VstFactory::VstFactory(std::string filesDir,
                       std::string wineRoot,
                       std::string assetsDir,
                       std::string nativeLibDir)
    : filesDir_(std::move(filesDir)),
      wineRoot_(std::move(wineRoot)),
      assetsDir_(std::move(assetsDir)),
      nativeLibDir_(std::move(nativeLibDir)) {}

VstFactory::~VstFactory() = default;

bool VstFactory::initialize() {
    LOGI("VstFactory: init filesDir=%s wineRoot=%s",
         filesDir_.c_str(), wineRoot_.c_str());
    return loadRegistry();
}

void VstFactory::refresh() { loadRegistry(); }

namespace {
// Minimal extractor for the very narrow registry.json schema we control
// end-to-end (writer is :app Kotlin in Phase D). NOT a general JSON parser.
// Schema:
//   {"plugins":[
//      {"uuid":"...","displayName":"...","format":"VST2","dllPath":"...","is64Bit":true},
//      ...
//   ]}
// We scan for object-open '{' inside "plugins":[ ... ] and pull the next
// string value for each named key, plus a true/false for is64Bit.
std::string extractString(const std::string& s, const std::string& key, size_t from) {
    const std::string needle = "\"" + key + "\"";
    size_t p = s.find(needle, from);
    if (p == std::string::npos) return {};
    p = s.find(':', p);
    if (p == std::string::npos) return {};
    p = s.find('"', p);
    if (p == std::string::npos) return {};
    ++p;
    std::string out;
    while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < s.size()) { out += s[p + 1]; p += 2; }
        else                                   { out += s[p];     ++p;    }
    }
    return out;
}
bool extractBool(const std::string& s, const std::string& key, size_t from, bool def) {
    const std::string needle = "\"" + key + "\"";
    size_t p = s.find(needle, from);
    if (p == std::string::npos) return def;
    p = s.find(':', p);
    if (p == std::string::npos) return def;
    while (p < s.size() && (s[p] == ' ' || s[p] == ':' || s[p] == '\t')) ++p;
    if (s.compare(p, 4, "true") == 0) return true;
    if (s.compare(p, 5, "false") == 0) return false;
    return def;
}
}  // namespace

bool VstFactory::loadRegistry() {
    const std::string path = filesDir_ + "/vst_plugins/registry.json";
    std::ifstream f(path);
    std::lock_guard<std::mutex> lk(mu_);
    entries_.clear();
    if (!f.is_open()) {
        LOGI("VstFactory: no registry at %s (no imported VSTs yet)", path.c_str());
        return true;  // not an error — just empty
    }
    std::stringstream ss; ss << f.rdbuf();
    const std::string body = ss.str();

    size_t arrStart = body.find("\"plugins\"");
    if (arrStart == std::string::npos) {
        LOGW("VstFactory: registry.json missing \"plugins\" key");
        return true;
    }
    arrStart = body.find('[', arrStart);
    if (arrStart == std::string::npos) return true;

    size_t cursor = arrStart + 1;
    while (cursor < body.size()) {
        size_t obj = body.find('{', cursor);
        if (obj == std::string::npos) break;
        size_t objEnd = body.find('}', obj);
        if (objEnd == std::string::npos) break;
        const size_t objLen = objEnd - obj + 1;
        const std::string objStr = body.substr(obj, objLen);

        RegistryEntry e;
        e.uuid        = extractString(objStr, "uuid",        0);
        e.displayName = extractString(objStr, "displayName", 0);
        e.format      = extractString(objStr, "format",      0);
        e.dllPath     = extractString(objStr, "dllPath",     0);
        e.is64Bit     = extractBool  (objStr, "is64Bit",     0, true);

        if (!e.uuid.empty() && !e.dllPath.empty()) {
            entries_.push_back(std::move(e));
        }
        cursor = objEnd + 1;
    }
    LOGI("VstFactory: loaded %zu plugin(s) from %s",
         entries_.size(), path.c_str());
    return true;
}

std::vector<guitarrackcraft::PluginInfo> VstFactory::enumeratePlugins() {
    std::vector<guitarrackcraft::PluginInfo> out;
    std::lock_guard<std::mutex> lk(mu_);
    out.reserve(entries_.size());
    for (const auto& e : entries_) {
        guitarrackcraft::PluginInfo info;
        // PluginRegistry::initializeAll prepends "<format>:" so id holds just
        // the uuid. Mirrors LV2PluginFactory which puts the LV2 URI here.
        info.id = e.uuid;
        info.name = e.displayName;
        info.format = e.format;
        // Stereo I/O (no params yet — they get learned at activate time via
        // the shm handshake). Surfaces matching ports so RackManager can
        // connect us into the chain.
        guitarrackcraft::PortInfo in_l { 0, "In L",  "in_l",  true,  true, false, false, 0, 0, 0, {} };
        guitarrackcraft::PortInfo in_r { 1, "In R",  "in_r",  true,  true, false, false, 0, 0, 0, {} };
        guitarrackcraft::PortInfo out_l{ 2, "Out L", "out_l", false, true, false, false, 0, 0, 0, {} };
        guitarrackcraft::PortInfo out_r{ 3, "Out R", "out_r", false, true, false, false, 0, 0, 0, {} };
        info.ports = { in_l, in_r, out_l, out_r };
        out.push_back(std::move(info));
    }
    return out;
}

std::unique_ptr<guitarrackcraft::IPlugin>
VstFactory::createPlugin(const std::string& pluginId) {
    // pluginId arrives stripped of the "VST2:" prefix by PluginRegistry — it's
    // the bare uuid.
    RegistryEntry match;
    int displayN = -1;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& e : entries_) {
            if (e.uuid == pluginId) { match = e; break; }
        }
        if (match.uuid.empty()) return nullptr;
        displayN = nextDisplayNumber_++;
    }
    return std::make_unique<WineVstPlugin>(
        std::move(match), filesDir_, wineRoot_, assetsDir_, nativeLibDir_, displayN);
}

std::unique_ptr<guitarrackcraft::IPluginFactory> createVstFactory(
    const std::string& filesDir,
    const std::string& wineRoot,
    const std::string& assetsDir,
    const std::string& nativeLibDir) {
    return std::make_unique<VstFactory>(filesDir, wineRoot, assetsDir, nativeLibDir);
}

} // namespace vsthost
