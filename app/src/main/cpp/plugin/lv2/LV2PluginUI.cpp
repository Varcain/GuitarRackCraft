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

#include "LV2PluginUI.h"
#include "../../x11/X11NativeDisplay.h"
#include <android/log.h>
#include <atomic>
#include <dlfcn.h>
#include <cstring>
#include <unistd.h>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cerrno>
#include <android/dlext.h>
#include <fcntl.h>

#define LOG_TAG "LV2PluginUI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Minimal LV2 UI types — keeps us independent of lv2/ui/ui.h at     */
/* compile time while still being ABI-compatible.                     */
/* ------------------------------------------------------------------ */

typedef void* LV2UI_Handle;
typedef void* LV2UI_Controller;
typedef void* LV2UI_Widget;

typedef void (*LV2UI_Write_Function)(
    LV2UI_Controller controller,
    uint32_t         port_index,
    uint32_t         buffer_size,
    uint32_t         port_protocol,
    const void*      buffer);

typedef struct {
    const char* URI;
    void*       data;
} LV2_Feature;

struct guitarrackcraft::LV2PluginUI::LV2UIDescriptor {
    const char*  URI;

    LV2UI_Handle (*instantiate)(
        const LV2UIDescriptor*  descriptor,
        const char*             plugin_uri,
        const char*             bundle_path,
        LV2UI_Write_Function    write_function,
        LV2UI_Controller        controller,
        LV2UI_Widget*           widget,
        const LV2_Feature* const* features);

    void (*cleanup)(LV2UI_Handle ui);

    void (*port_event)(
        LV2UI_Handle ui,
        uint32_t     port_index,
        uint32_t     buffer_size,
        uint32_t     format,
        const void*  buffer);

    const void* (*extension_data)(const char* uri);
};

typedef const guitarrackcraft::LV2PluginUI::LV2UIDescriptor*
    (*LV2UI_DescriptorFunction)(uint32_t index);

/* Idle interface (LV2 UI extension) */
struct LV2UI_Idle_Interface {
    int (*idle)(LV2UI_Handle ui);
};

/* Resize interface (LV2 UI extension) */
typedef void* LV2UI_Feature_Handle;
struct LV2UI_Resize {
    LV2UI_Feature_Handle handle;
    int (*ui_resize)(LV2UI_Feature_Handle handle, int width, int height);
};

/* Minimal URID types (ABI-compatible with lv2/urid/urid.h) */
typedef uint32_t LV2_URID;
typedef void*    LV2_URID_Map_Handle;
struct LV2_URID_Map {
    LV2_URID_Map_Handle handle;
    LV2_URID (*map)(LV2_URID_Map_Handle handle, const char* uri);
};

/* Minimal LV2 Options types (ABI-compatible with lv2/options/options.h) */
struct LV2_Options_Option {
    uint32_t context;  // LV2_Options_Context
    uint32_t subject;
    LV2_URID key;
    uint32_t size;
    LV2_URID type;
    const void* value;
};

/* Minimal URID Unmap (ABI-compatible with lv2/urid/urid.h) */
typedef void* LV2_URID_Unmap_Handle;
struct LV2_URID_Unmap {
    LV2_URID_Unmap_Handle handle;
    const char* (*unmap)(LV2_URID_Unmap_Handle handle, LV2_URID urid);
};

/* Minimal ui:requestValue types (ABI-compatible with lv2/ui/ui.h) */
typedef uint32_t LV2UI_Request_Value_Status;
#define LV2UI_REQUEST_VALUE_SUCCESS 0
#define LV2UI_REQUEST_VALUE_ERR_UNKNOWN 1
struct LV2UI_Request_Value {
    LV2UI_Feature_Handle handle;
    LV2UI_Request_Value_Status (*request)(LV2UI_Feature_Handle handle,
                                          LV2_URID key, LV2_URID type,
                                          const LV2_Feature* const* features);
};

/* Minimal ui:portMap types (ABI-compatible with lv2/ui/ui.h) */
#define LV2UI_INVALID_PORT_INDEX ((uint32_t)-1)
struct LV2UI_Port_Map {
    LV2UI_Feature_Handle handle;
    uint32_t (*port_index)(LV2UI_Feature_Handle handle, const char* symbol);
};

// Forward-declare the global URID map/unmap from LV2Plugin.cpp
// so that UI and DSP share the same URID numbering.
extern LV2_URID_Map globalLv2UridMap;
extern LV2_URID_Unmap globalLv2UridUnmap;

/* UI URID map — delegates to the global (DSP) map so both sides produce
   identical URIDs.  This is critical for atom-based communication (e.g. DPF
   state sync for meter resets, file path delivery). */
namespace {
struct SimpleUridMap {
    LV2_URID map(const char* uri) {
        return globalLv2UridMap.map(globalLv2UridMap.handle, uri);
    }
    const char* unmap(LV2_URID id) {
        return globalLv2UridUnmap.unmap(globalLv2UridUnmap.handle, id);
    }
};

SimpleUridMap& getUIUridMap() {
    static SimpleUridMap instance;
    return instance;
}

LV2_URID uiUridMapCallback(LV2_URID_Map_Handle /*handle*/, const char* uri) {
    return globalLv2UridMap.map(globalLv2UridMap.handle, uri);
}

LV2_URID_Map uiLv2UridMap = { nullptr, uiUridMapCallback };

const char* uiUridUnmapCallback(LV2_URID_Unmap_Handle /*handle*/, LV2_URID id) {
    return globalLv2UridUnmap.unmap(globalLv2UridUnmap.handle, id);
}

LV2_URID_Unmap uiLv2UridUnmap = { nullptr, uiUridUnmapCallback };

} // anonymous namespace

static LV2UI_Request_Value_Status requestValueCallback(
    LV2UI_Feature_Handle handle, LV2_URID key, LV2_URID /*type*/,
    const LV2_Feature* const* /*features*/)
{
    auto* self = static_cast<guitarrackcraft::LV2PluginUI*>(handle);
    if (!self) return LV2UI_REQUEST_VALUE_ERR_UNKNOWN;

    const char* keyUri = getUIUridMap().unmap(key);
    if (!keyUri) {
        LOGE("requestValueCallback: cannot unmap key URID %u", key);
        return LV2UI_REQUEST_VALUE_ERR_UNKNOWN;
    }

    LOGI("requestValueCallback: key=%s (urid=%u)", keyUri, key);
    self->setPendingFileRequest(keyUri);

    return LV2UI_REQUEST_VALUE_SUCCESS;
}

/* Well-known URIs */
static const char* LV2_UI__parent        = "http://lv2plug.in/ns/extensions/ui#parent";
static const char* LV2_UI__idleInterface  = "http://lv2plug.in/ns/extensions/ui#idleInterface";
static const char* LV2_UI__resize         = "http://lv2plug.in/ns/extensions/ui#resize";
static const char* LV2_URID__map         = "http://lv2plug.in/ns/ext/urid#map";
static const char* LV2_OPTIONS__options   = "http://lv2plug.in/ns/ext/options#options";
static const char* LV2_PARAMETERS__sampleRate = "http://lv2plug.in/ns/ext/parameters#sampleRate";
static const char* LV2_ATOM__Float        = "http://lv2plug.in/ns/ext/atom#Float";
static const char* LV2_UI__scaleFactor    = "http://lv2plug.in/ns/extensions/ui#scaleFactor";
static const char* LV2_URID__unmap       = "http://lv2plug.in/ns/ext/urid#unmap";
static const char* LV2_UI__requestValue  = "http://lv2plug.in/ns/extensions/ui#requestValue";
static const char* LV2_UI__portMap       = "http://lv2plug.in/ns/extensions/ui#portMap";
static const char* LV2_PATCH__Set        = "http://lv2plug.in/ns/ext/patch#Set";
static const char* LV2_PATCH__property   = "http://lv2plug.in/ns/ext/patch#property";
static const char* LV2_PATCH__value      = "http://lv2plug.in/ns/ext/patch#value";
static const char* LV2_ATOM__Object      = "http://lv2plug.in/ns/ext/atom#Object";
static const char* LV2_ATOM__URID        = "http://lv2plug.in/ns/ext/atom#URID";
static const char* LV2_ATOM__Path        = "http://lv2plug.in/ns/ext/atom#Path";
static const char* LV2_ATOM__eventTransfer = "http://lv2plug.in/ns/ext/atom#eventTransfer";

namespace guitarrackcraft {

LV2PluginUI::LV2PluginUI() = default;

LV2PluginUI::~LV2PluginUI() {
    cleanup();
}

/* ------------------------------------------------------------------ */
/* Static write callback — forwarded to the ParameterCallback.        */
/* Also handles atom messages (DPF state sync, patch:Set).            */
/* ------------------------------------------------------------------ */
void LV2PluginUI::writeFunction(void* controller,
                                uint32_t portIndex,
                                uint32_t bufferSize,
                                uint32_t portProtocol,
                                const void* buffer)
{
    if (!buffer) return;

    auto* self = static_cast<LV2PluginUI*>(controller);

    if (bufferSize == sizeof(float) && portProtocol == 0) {
        /* Float control port write */
        float value = *static_cast<const float*>(buffer);
        if (self->paramCb_) {
            self->paramCb_(portIndex, value);
        }
    } else if (portProtocol != 0 && bufferSize > 0) {
        /* Atom message (e.g. DPF state sync, patch:Set) — forward to DSP */
        if (self->atomCb_) {
            self->atomCb_(portIndex, bufferSize, buffer);
        }
    }
}

/* ------------------------------------------------------------------ */
/* instantiate                                                        */
/* ------------------------------------------------------------------ */
/* Android 10+ restricts dlopen() from the app's writable directories.
   This helper falls back to android_dlopen_ext() with a file descriptor
   to bypass the linker namespace path restriction. */
static void* dlopen_compat(const char* path, int flags) {
    void* h = dlopen(path, flags);
    if (!h) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            android_dlextinfo extinfo;
            memset(&extinfo, 0, sizeof(extinfo));
            extinfo.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
            extinfo.library_fd = fd;
            dlerror();
            h = android_dlopen_ext(path, flags, &extinfo);
            close(fd);
        }
    }
    return h;
}

static bool copyFile(const std::string& src, const std::string& dest) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dest, std::ios::binary);
    if (!out) return false;
    out << in.rdbuf();
    return out.good();
}

bool LV2PluginUI::instantiate(
    const std::string& uiBinaryPath,
    const std::string& uiUri,
    const std::string& pluginUri,
    int displayNumber,
    unsigned long parentWindowId,
    IPlugin* plugin,
    ParameterCallback paramCallback,
    const std::string& nativeLibDir,
    const std::string& x11LibsDir)
{
    LOGI("instantiate: [1] ENTER path=%s display=%d parent=0x%lx", uiBinaryPath.c_str(), displayNumber, parentWindowId);

    /* Preload libX11 from app lib dir so plugin .so can resolve XOpenDisplay; use absolute path so linker finds it. */
    void* x11Handle = nullptr;
    if (!nativeLibDir.empty()) {
        std::string x11Path = nativeLibDir;
        if (x11Path.back() != '/') x11Path += '/';
        x11Path += "libX11.so";
        x11Handle = dlopen(x11Path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    }
    if (!x11Handle)
        x11Handle = dlopen("libX11.so", RTLD_NOW | RTLD_GLOBAL);
    LOGI("instantiate: libX11 preload %s", x11Handle ? "ok" : "not found (plugin may have X11 statically linked)");

    plugin_.store(plugin, std::memory_order_release);
    paramCb_ = std::move(paramCallback);
    libCopyPath_.clear();

    /* --- Set DISPLAY so Xlib inside the UI .so connects to the minimal X11 server.
           Always use TCP (127.0.0.1:N) because our custom libxcb doesn't support
           abstract Unix sockets and /tmp/.X11-unix/ doesn't exist on Android. */
    std::string displayEnv = "127.0.0.1:" + std::to_string(displayNumber) + ".0";
    setenv("DISPLAY", displayEnv.c_str(), 1 /*overwrite*/);
    LOGI("instantiate: DISPLAY=%s", displayEnv.c_str());
    
    /* Set XCB threading options to be more tolerant.
     * LIBXCB_ALLOW_SLOPPY_LOCK allows XCB to work even with some threading issues.
     * This helps prevent the "xcb_xlib_threads_sequence_lost" assertion failure
     * when the plugin uses X11 from multiple threads internally. */
    setenv("LIBXCB_ALLOW_SLOPPY_LOCK", "1", 1);
    
    LOGI("instantiate: [2] DISPLAY=%s LIBXCB_ALLOW_SLOPPY_LOCK=1", displayEnv.c_str());

    /* Extract base name and bundle dir from uiBinaryPath */
    const size_t lastSlash = uiBinaryPath.find_last_of('/');
    const std::string bundleDir = (lastSlash != std::string::npos)
        ? uiBinaryPath.substr(0, lastSlash + 1) : std::string("./");

    /* Preload X11 libs so dlopen(plugin_ui.so) can resolve DT_NEEDED (libxcb.so etc.).
     * Try nativeLibDir first (unversioned, standard), then x11LibsDir as fallback.
     * Load in dependency order: Xau, xcb, X11. */
    {
        std::string natDir = nativeLibDir;
        if (!natDir.empty() && natDir.back() != '/') natDir += '/';
        std::string x11Dir = x11LibsDir;
        if (!x11Dir.empty() && x11Dir.back() != '/') x11Dir += '/';

        const char* preloadOrder[][2] = {
            {"libXau.so",  "libXau.so.6"},
            {"libxcb.so",  "libxcb.so.1"},
            {"libX11.so",  "libX11.so.6"},
            {nullptr, nullptr}
        };
        for (int i = 0; preloadOrder[i][0]; ++i) {
            void* h = nullptr;
            /* Try unversioned from nativeLibDir (standard path after build.sh rename) */
            if (!h && !natDir.empty())
                h = dlopen_compat((natDir + preloadOrder[i][0]).c_str(), RTLD_NOW | RTLD_GLOBAL);
            /* Try versioned from nativeLibDir */
            if (!h && !natDir.empty())
                h = dlopen_compat((natDir + preloadOrder[i][1]).c_str(), RTLD_NOW | RTLD_GLOBAL);
            /* Try from x11LibsDir (scratch dir fallback) */
            if (!h && !x11Dir.empty())
                h = dlopen_compat((x11Dir + preloadOrder[i][0]).c_str(), RTLD_NOW | RTLD_GLOBAL);
            if (!h && !x11Dir.empty())
                h = dlopen_compat((x11Dir + preloadOrder[i][1]).c_str(), RTLD_NOW | RTLD_GLOBAL);
            if (h && strcmp(preloadOrder[i][0], "libX11.so") == 0)
                x11Handle = h;
            LOGI("instantiate: preload %s %s", preloadOrder[i][0], h ? "ok" : "skip");
        }

        /* NOTE: Do NOT call XInitThreads(). We guarantee single-threaded Display* access
         * via the pluginUI thread. XInitThreads enables XCB's sequence tracking assertions
         * which have known bugs in the Xlib-XCB bridge (xcb_xlib_threads_sequence_lost)
         * that fire even with correct single-threaded usage. */
    }

    /* Prefer loading from the given path (extracted assets under files/lv2/...). Plugin UI
       .so files are not in the app lib dir; only the main app .so and optional provider
       are. If loading from uiBinaryPath fails (e.g. missing _binary_pedal_png_*), we can
       try copying to nativeLibDir and loading from there so symbols resolve. */
    const std::string baseName = (lastSlash != std::string::npos)
        ? uiBinaryPath.substr(lastSlash + 1) : uiBinaryPath;
    std::string pathToOpen = uiBinaryPath;
    void* handle = nullptr;

    /* 1) If we have x11LibsDir, copy plugin there and load so linker finds X11 libs in same dir.
       Use display number in filename to avoid collisions when multiple instances of the same
       plugin are loaded — otherwise cleanup of one instance can unlink the .so while another
       instance still has it memory-mapped, causing SIGBUS. */
    if (!handle && !x11LibsDir.empty()) {
        std::string x11Dir = x11LibsDir;
        if (!x11Dir.empty() && x11Dir.back() != '/') x11Dir += '/';
        std::string destPath = x11Dir + "plugin_ui_d" + std::to_string(displayNumber) + "_" + baseName;
        if (copyFile(uiBinaryPath, destPath)) {
            handle = dlopen_compat(destPath.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (handle) {
                libCopyPath_ = destPath;
                pathToOpen = destPath;
                LOGI("instantiate: loaded UI .so from x11 libs dir: %s", destPath.c_str());
            }
        }
    }
    /* 2) Try the path we were given (bundle). SONAME libs preloaded above so DT_NEEDED may resolve. */
    if (!handle) {
        handle = dlopen_compat(uiBinaryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle)
            LOGI("instantiate: loaded UI .so from bundle path: %s", uiBinaryPath.c_str());
    }

    /* 3) If that failed and we have nativeLibDir, try copy-to-lib-dir so _binary_pedal_png_*
       from the provider (same namespace) can resolve. */
    if (!handle && !nativeLibDir.empty()) {
        std::string destPath = nativeLibDir;
        if (destPath.back() != '/') destPath += '/';
        destPath += "plugin_ui_d" + std::to_string(displayNumber) + "_";
        destPath += baseName;
        std::ifstream in(uiBinaryPath, std::ios::binary);
        if (in) {
            std::ofstream out(destPath, std::ios::binary);
            if (out) {
                out << in.rdbuf();
                out.close();
                in.close();
                if (out.good()) {
                    libCopyPath_ = destPath;
                    pathToOpen = destPath;
                    handle = dlopen_compat(pathToOpen.c_str(), RTLD_NOW | RTLD_LOCAL);
                    if (handle) {
                        LOGI("instantiate: copied UI .so to lib dir and loaded: %s", destPath.c_str());
                    }
                }
            }
        }
        if (!handle && !libCopyPath_.empty()) {
            (void)unlink(libCopyPath_.c_str());
            libCopyPath_.clear();
        }
    }

    /* 4) Fallback: try original path again (e.g. if pathToOpen was changed but dlopen failed). */
    if (!handle) {
        pathToOpen = uiBinaryPath;
        handle = dlopen_compat(pathToOpen.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            LOGI("instantiate: loaded UI .so from bundle path (fallback): %s", pathToOpen.c_str());
        }
    }

    libHandle_ = handle;
    LOGI("instantiate: [3] dlopen UI .so done handle=%p", (void*)handle);
    if (!libHandle_) {
        LOGE("dlopen(%s): %s", pathToOpen.c_str(), dlerror());
        if (!libCopyPath_.empty()) {
            (void)unlink(libCopyPath_.c_str());
            libCopyPath_.clear();
        }
        return false;
    }

    auto getDescriptor =
        reinterpret_cast<LV2UI_DescriptorFunction>(
            dlsym(libHandle_, "lv2ui_descriptor"));
    if (!getDescriptor) {
        LOGE("lv2ui_descriptor not found: %s", dlerror());
        cleanup();
        return false;
    }

    /* --- Find the descriptor whose URI matches ------------------- */
    desc_ = nullptr;
    for (uint32_t idx = 0; ; ++idx) {
        auto* d = getDescriptor(idx);
        if (!d) break;
        if (d->URI && uiUri == d->URI) {
            desc_ = d;
            break;
        }
    }
    if (!desc_) {
        LOGE("No UI descriptor matching URI %s", uiUri.c_str());
        cleanup();
        return false;
    }
    if (!desc_->instantiate) {
        LOGE("UI descriptor has null instantiate for URI %s", uiUri.c_str());
        cleanup();
        return false;
    }
    LOGI("instantiate: [4] descriptor found for URI %s", uiUri.c_str());

    /* Debug: check if plugin .so resolves XOpenDisplay (indicates X11 UI code is present) */
    void* xopenSym = dlsym(libHandle_, "XOpenDisplay");
    LOGI("instantiate: plugin .so XOpenDisplay %s", xopenSym ? "resolved" : "not found (may be static)");

    /* --- Build LV2 feature list ---------------------------------- */
    LV2_Feature parentFeature;
    parentFeature.URI  = LV2_UI__parent;
    parentFeature.data = reinterpret_cast<void*>(parentWindowId);

    /* Provide a resize feature so plugins can request host window resize.
       Our implementation is a no-op since plugin windows are fixed-size. */
    static auto resizeCallback = [](LV2UI_Feature_Handle /*handle*/, int /*w*/, int /*h*/) -> int {
        return 0;
    };
    LV2UI_Resize resizeData;
    resizeData.handle = nullptr;
    resizeData.ui_resize = resizeCallback;

    LV2_Feature resizeFeature;
    resizeFeature.URI  = LV2_UI__resize;
    resizeFeature.data = &resizeData;

    /* URID map — needed by DPF UI for options parsing and atom communication */
    LV2_Feature uridFeature;
    uridFeature.URI  = LV2_URID__map;
    uridFeature.data = &uiLv2UridMap;

    /* Options — provide sample rate and scale factor so DPF UIs can query them.
       Scale factor is critical: without it, DPF opens a SECOND XOpenDisplay()
       to detect DPI, which hangs because our X11 server only handles one client. */
    float sampleRate = 48000.0f;
    float uiScaleFactor = 1.0f;
    LV2_URID uridFloat = uiLv2UridMap.map(uiLv2UridMap.handle, LV2_ATOM__Float);
    LV2_URID uridSampleRate = uiLv2UridMap.map(uiLv2UridMap.handle, LV2_PARAMETERS__sampleRate);
    LV2_URID uridScaleFactor = uiLv2UridMap.map(uiLv2UridMap.handle, LV2_UI__scaleFactor);
    LV2_Options_Option optionsList[] = {
        { 0 /*instance*/, 0, uridSampleRate,  sizeof(float), uridFloat, &sampleRate },
        { 0 /*instance*/, 0, uridScaleFactor, sizeof(float), uridFloat, &uiScaleFactor },
        { 0, 0, 0, 0, 0, nullptr }  // sentinel
    };
    LV2_Feature optionsFeature;
    optionsFeature.URI  = LV2_OPTIONS__options;
    optionsFeature.data = optionsList;

    /* Idle interface — declare support so DPF knows we'll call idle() */
    LV2_Feature idleFeature;
    idleFeature.URI  = LV2_UI__idleInterface;
    idleFeature.data = nullptr;

    /* URID unmap — needed by DPF for unmapping URIDs back to URIs */
    LV2_Feature uridUnmapFeature;
    uridUnmapFeature.URI  = LV2_URID__unmap;
    uridUnmapFeature.data = &uiLv2UridUnmap;

    /* ui:requestValue — lets the plugin UI request file paths from the host.
       Heap-allocated because the plugin stores a pointer to this struct and uses
       it later (during idle/event processing), long after instantiate() returns. */
    auto* rvData = new LV2UI_Request_Value{this, requestValueCallback};
    requestValueData_ = rvData;
    LV2_Feature requestValueFeature;
    requestValueFeature.URI  = LV2_UI__requestValue;
    requestValueFeature.data = rvData;

    /* ui:portMap — maps port symbols to LV2 port indices.  Critical for DPF
       to find the bypass/enabled port and apply the value inversion.  Without
       this, DPF's fBypassParameterIndex stays LV2UI_INVALID_PORT_INDEX and the
       bypass toggle is completely backwards. */
    IPlugin* pluginPtrForMap = plugin_.load(std::memory_order_acquire);
    if (pluginPtrForMap) {
        PluginInfo mapInfo = pluginPtrForMap->getInfo();
        portSymbolMap_.clear();
        for (const auto& port : mapInfo.ports) {
            if (!port.symbol.empty()) {
                portSymbolMap_[port.symbol] = port.index;
            }
        }
    }
    auto portMapCb = [](LV2UI_Feature_Handle handle, const char* symbol) -> uint32_t {
        if (!handle || !symbol) return LV2UI_INVALID_PORT_INDEX;
        auto* self = static_cast<LV2PluginUI*>(handle);
        auto it = self->portSymbolMap_.find(symbol);
        if (it != self->portSymbolMap_.end()) return it->second;
        return LV2UI_INVALID_PORT_INDEX;
    };
    portMapData_ = {this, portMapCb};

    LV2_Feature portMapFeature;
    portMapFeature.URI  = LV2_UI__portMap;
    portMapFeature.data = &portMapData_;

    const LV2_Feature* features[] = {
        &parentFeature, &resizeFeature, &uridFeature,
        &uridUnmapFeature, &requestValueFeature,
        &optionsFeature, &idleFeature, &portMapFeature, nullptr
    };

    /* --- Instantiate --------------------------------------------- */
    LV2UI_Widget widget = nullptr;

    /* bundle_path: directory containing the UI binary */
    std::string bundlePath = uiBinaryPath;
    auto bundleSlash = bundlePath.rfind('/');
    if (bundleSlash != std::string::npos) {
        bundlePath = bundlePath.substr(0, bundleSlash + 1);
    }

    LOGI("instantiate: [7] calling plugin instantiate() uri=%s DISPLAY=%s parent=0x%lx bundlePath=%s",
         pluginUri.c_str(), displayEnv.c_str(), parentWindowId, bundlePath.c_str());

    uiHandle_ = desc_->instantiate(
        desc_,
        pluginUri.c_str(),
        bundlePath.c_str(),
        reinterpret_cast<LV2UI_Write_Function>(&writeFunction),
        static_cast<void*>(this),   /* controller */
        &widget,
        features);

    LOGI("instantiate: [8] plugin instantiate() RETURNED handle=%p widget=%p", uiHandle_, widget);
    if (!uiHandle_) {
        const char* err = dlerror();
        LOGE("UI instantiate() returned null for %s", uiUri.c_str());
        if (err) LOGE("dlerror() after instantiate: %s", err);
        cleanup();
        return false;
    }

    /* --- Cache idle and resize function pointers -------------------- */
    if (desc_->extension_data) {
        auto* idleIface = static_cast<const LV2UI_Idle_Interface*>(
            desc_->extension_data(LV2_UI__idleInterface));
        if (idleIface && idleIface->idle) {
            idleFn_ = reinterpret_cast<int(*)(void*)>(
                reinterpret_cast<void*>(idleIface->idle));
        }
        auto* resizeIface = static_cast<const LV2UI_Resize*>(
            desc_->extension_data(LV2_UI__resize));
        if (resizeIface && resizeIface->ui_resize) {
            resizeFn_ = reinterpret_cast<int(*)(void*, int, int)>(
                reinterpret_cast<void*>(resizeIface->ui_resize));
            LOGI("instantiate: cached resize function from extension_data");
        }
    }

    /* --- Send initial port values to the UI ---------------------- */
    IPlugin* pluginPtr = plugin_.load(std::memory_order_acquire);
    if (pluginPtr) {
        PluginInfo info = pluginPtr->getInfo();
        int portEventCount = 0;
        for (const auto& port : info.ports) {
            if (port.isControl && !port.isAudio) {
                float val = pluginPtr->getParameter(port.index);
                portEvent(port.index, val);
                portEventCount++;
                if (!port.isInput) {
                    outputControlPorts_.push_back(port.index);
                    lastOutputValues_[port.index] = val;
                }
            }
        }
        LOGI("instantiate: [9] sent %d port values to UI (%zu output control ports)",
             portEventCount, outputControlPorts_.size());
    }

    LOGI("instantiate: [10] UI ready uri=%s display=%s parent=0x%lx", uiUri.c_str(), displayEnv.c_str(), parentWindowId);
    return true;
}

void LV2PluginUI::triggerResize() {
    if (resizeFn_ && uiHandle_) {
        LOGI("triggerResize: calling plugin resize fn");
        resizeFn_(uiHandle_, 0, 0);  // w,h ignored by plugin - it reads XGetWindowAttributes
    }
}

/* ------------------------------------------------------------------ */
/* cleanup                                                            */
/* ------------------------------------------------------------------ */
void LV2PluginUI::cleanup() {
    LOGI("cleanup: ENTER uiHandle=%p desc=%p", (void*)uiHandle_, (void*)desc_);
    
    if (uiHandle_ && desc_ && desc_->cleanup) {
        LOGI("cleanup: calling plugin cleanup(uiHandle)");
        desc_->cleanup(static_cast<LV2UI_Handle>(uiHandle_));
        LOGI("cleanup: plugin cleanup returned");
    }
    plugin_.store(nullptr, std::memory_order_release);
    uiHandle_ = nullptr;
    desc_     = nullptr;
    idleFn_   = nullptr;
    resizeFn_ = nullptr;
    outputControlPorts_.clear();
    lastOutputValues_.clear();

    if (requestValueData_) {
        delete static_cast<LV2UI_Request_Value*>(requestValueData_);
        requestValueData_ = nullptr;
    }

    if (libHandle_) {
        LOGI("cleanup: dlclose(plugin .so) handle=%p", (void*)libHandle_);
        dlclose(libHandle_);
        libHandle_ = nullptr;
        LOGI("cleanup: dlclose done");
    }
    if (!libCopyPath_.empty()) {
        (void)unlink(libCopyPath_.c_str());
        libCopyPath_.clear();
    }
    LOGI("cleanup: EXIT");
}

/* ------------------------------------------------------------------ */
/* idle                                                               */
/* ------------------------------------------------------------------ */
int LV2PluginUI::idle() {
    if (!idleFn_ || !uiHandle_ || !isValid()) {
        return 0;
    }
    return idleFn_(uiHandle_);
}

/* ------------------------------------------------------------------ */
/* portEvent — forward a value change to the UI                       */
/* ------------------------------------------------------------------ */
void LV2PluginUI::portEvent(uint32_t portIndex, float value) {
    static std::atomic<int> portEventLogCount{0};
    int c = portEventLogCount++;
    if (c < 5) {
        LOGI("portEvent: port=%u value=%.4f", portIndex, value);
    }
    if (!uiHandle_ || !desc_ || !desc_->port_event || !isValid()) {
        return;
    }
    
    desc_->port_event(
        static_cast<LV2UI_Handle>(uiHandle_),
        portIndex,
        sizeof(float),
        0,          /* format 0 = float */
        &value);
}

/* ------------------------------------------------------------------ */
/* portEventAtom — forward a DSP atom event to the UI                 */
/* ------------------------------------------------------------------ */
void LV2PluginUI::portEventAtom(uint32_t portIndex, uint32_t size, const void* data) {
    if (!uiHandle_ || !desc_ || !desc_->port_event || !isValid() || !data || size == 0) {
        return;
    }
    auto& uridMap = getUIUridMap();
    LV2_URID eventTransfer = uridMap.map(LV2_ATOM__eventTransfer);

    desc_->port_event(
        static_cast<LV2UI_Handle>(uiHandle_),
        portIndex,
        size,
        eventTransfer,
        data);
}

/* ------------------------------------------------------------------ */
/* syncOutputPorts — poll output control ports and push to UI         */
/* ------------------------------------------------------------------ */
void LV2PluginUI::syncOutputPorts(std::shared_mutex* chainMutex) {
    // Take a shared lock on the chain mutex to prevent reorder from
    // moving/destroying plugin objects while we read port values.
    std::shared_lock<std::shared_mutex> lock;
    if (chainMutex) {
        lock = std::shared_lock<std::shared_mutex>(*chainMutex, std::try_to_lock);
        if (!lock.owns_lock()) return;  // reorder in progress, skip this cycle
    }

    IPlugin* p = plugin_.load(std::memory_order_acquire);
    if (!p || !isValid()) return;
    for (uint32_t portIndex : outputControlPorts_) {
        float val = p->getParameter(portIndex);
        auto it = lastOutputValues_.find(portIndex);
        if (it == lastOutputValues_.end() || it->second != val) {
            lastOutputValues_[portIndex] = val;
            portEvent(portIndex, val);
        }
    }

    // Forward atom output events from DSP to UI
    auto atoms = p->drainOutputAtoms();
    for (auto& atom : atoms) {
        portEventAtom(atom.portIndex, static_cast<uint32_t>(atom.data.size()), atom.data.data());
    }
}

/* ------------------------------------------------------------------ */
/* getPendingFileRequest — return and clear any pending file request   */
/* ------------------------------------------------------------------ */
std::string LV2PluginUI::getPendingFileRequest() {
    std::lock_guard<std::mutex> lk(fileRequestMutex_);
    std::string uri;
    uri.swap(pendingFileRequestUri_);
    return uri;
}

/* ------------------------------------------------------------------ */
/* setPendingFileRequest — store a pending file request URI            */
/* ------------------------------------------------------------------ */
void LV2PluginUI::setPendingFileRequest(const std::string& uri) {
    std::lock_guard<std::mutex> lk(fileRequestMutex_);
    pendingFileRequestUri_ = uri;
}

/* ------------------------------------------------------------------ */
/* deliverFilePath — send a patch:Set atom to the UI via port_event   */
/* ------------------------------------------------------------------ */
void LV2PluginUI::deliverFilePath(const std::string& propertyUri, const std::string& filePath) {
    if (!uiHandle_ || !desc_ || !desc_->port_event || !isValid()) {
        LOGE("deliverFilePath: UI not valid, cannot deliver");
        return;
    }

    /* Map all URIDs we need */
    auto& uridMap = getUIUridMap();
    LV2_URID atomObjectUrid  = uridMap.map(LV2_ATOM__Object);
    LV2_URID patchSetUrid    = uridMap.map(LV2_PATCH__Set);
    LV2_URID patchPropUrid   = uridMap.map(LV2_PATCH__property);
    LV2_URID patchValueUrid  = uridMap.map(LV2_PATCH__value);
    LV2_URID atomUridType    = uridMap.map(LV2_ATOM__URID);
    LV2_URID atomPathType    = uridMap.map(LV2_ATOM__Path);
    LV2_URID eventTransfer   = uridMap.map(LV2_ATOM__eventTransfer);
    LV2_URID propertyUrid    = uridMap.map(propertyUri.c_str());

    /* Build a patch:Set atom object manually.
     *
     * Layout (LV2 Atom Object):
     *   [Atom header: size, type=atomObject]
     *   [Object body: id=0, otype=patchSet]
     *   [Property 1: key=patchProperty, context=0, value=(URID atom)]
     *     [Atom header: size=4, type=atomUrid]
     *     [uint32_t: propertyUrid]
     *     [4 bytes padding to align to 8]
     *   [Property 2: key=patchValue, context=0, value=(Path atom)]
     *     [Atom header: size=pathLen, type=atomPath]
     *     [char[]: filePath including null terminator]
     */

    /* Atom header: uint32_t size + uint32_t type */
    struct AtomHeader { uint32_t size; uint32_t type; };
    /* Object body: uint32_t id + uint32_t otype (follows atom header) */
    struct ObjectBody { uint32_t id; uint32_t otype; };
    /* Property body: uint32_t key + uint32_t context (then atom value follows) */
    struct PropertyBody { uint32_t key; uint32_t context; };

    uint32_t pathLen = static_cast<uint32_t>(filePath.size() + 1);  // include null
    uint32_t pathPadded = (pathLen + 7u) & ~7u;  // pad to 8-byte boundary

    /* Property 1 (patch:property → URID): header(8) + body(8) + atom(8) + value(4) + pad(4) = 32 */
    uint32_t prop1Size = sizeof(PropertyBody) + sizeof(AtomHeader) + sizeof(uint32_t);
    uint32_t prop1Padded = (prop1Size + 7u) & ~7u;

    /* Property 2 (patch:value → Path): header(8) + body(8) + atom(8) + pathLen */
    uint32_t prop2Size = sizeof(PropertyBody) + sizeof(AtomHeader) + pathLen;

    /* Total object body size: ObjectBody(8) + prop1Padded + prop2Size */
    uint32_t objectBodySize = sizeof(ObjectBody) + prop1Padded + prop2Size;

    /* Total allocation: AtomHeader(8) + objectBodySize */
    uint32_t totalSize = sizeof(AtomHeader) + objectBodySize;

    std::vector<uint8_t> buf(totalSize, 0);
    uint8_t* ptr = buf.data();

    /* Object atom header */
    auto* objAtom = reinterpret_cast<AtomHeader*>(ptr);
    objAtom->size = objectBodySize;
    objAtom->type = atomObjectUrid;
    ptr += sizeof(AtomHeader);

    /* Object body */
    auto* objBody = reinterpret_cast<ObjectBody*>(ptr);
    objBody->id = 0;
    objBody->otype = patchSetUrid;
    ptr += sizeof(ObjectBody);

    /* Property 1: patch:property */
    auto* prop1Body = reinterpret_cast<PropertyBody*>(ptr);
    prop1Body->key = patchPropUrid;
    prop1Body->context = 0;
    ptr += sizeof(PropertyBody);

    auto* prop1Atom = reinterpret_cast<AtomHeader*>(ptr);
    prop1Atom->size = sizeof(uint32_t);
    prop1Atom->type = atomUridType;
    ptr += sizeof(AtomHeader);

    *reinterpret_cast<uint32_t*>(ptr) = propertyUrid;
    ptr += sizeof(uint32_t);

    /* Pad prop1 to 8-byte alignment */
    uint32_t prop1Written = sizeof(PropertyBody) + sizeof(AtomHeader) + sizeof(uint32_t);
    if (prop1Padded > prop1Written) {
        ptr += (prop1Padded - prop1Written);
    }

    /* Property 2: patch:value */
    auto* prop2Body = reinterpret_cast<PropertyBody*>(ptr);
    prop2Body->key = patchValueUrid;
    prop2Body->context = 0;
    ptr += sizeof(PropertyBody);

    auto* prop2Atom = reinterpret_cast<AtomHeader*>(ptr);
    prop2Atom->size = pathLen;
    prop2Atom->type = atomPathType;
    ptr += sizeof(AtomHeader);

    memcpy(ptr, filePath.c_str(), pathLen);

    LOGI("deliverFilePath: sending patch:Set atom (prop=%s path=%s totalSize=%u)",
         propertyUri.c_str(), filePath.c_str(), totalSize);

    desc_->port_event(
        static_cast<LV2UI_Handle>(uiHandle_),
        0,          /* control/atom port (port 0 for DPF) */
        totalSize,
        eventTransfer,
        buf.data());
}

/* ------------------------------------------------------------------ */
/* isValid - check if UI handle is still valid                        */
/* ------------------------------------------------------------------ */
bool LV2PluginUI::isValid() const {
    return uiHandle_ != nullptr;
}

/* ------------------------------------------------------------------ */
/* gracefulShutdown - Phase 1: Tell plugin to stop, Phase 2: Wait     */
/* ------------------------------------------------------------------ */
bool LV2PluginUI::gracefulShutdown(int displayNumber, int timeoutMs) {
    LOGI("gracefulShutdown: ENTER display=%d timeout=%dms", displayNumber, timeoutMs);
    
    if (!uiHandle_) {
        LOGI("gracefulShutdown: no UI handle, nothing to do");
        return true;
    }
    
    // Phase 1: Signal the plugin to stop its event loop
    // The plugin's cleanup() function should stop the event loop
    // We call it here in Phase 1, before closing the X connection
    LOGI("gracefulShutdown: Phase 1 - calling plugin cleanup() to stop event loop");
    
    if (desc_ && desc_->cleanup) {
        LOGI("gracefulShutdown: calling plugin cleanup(uiHandle=%p)", (void*)uiHandle_);
        desc_->cleanup(static_cast<LV2UI_Handle>(uiHandle_));
        LOGI("gracefulShutdown: plugin cleanup() returned");
    }
    
    // Clear the handle so idle() won't be called anymore
    uiHandle_ = nullptr;
    
    // Phase 2: Wait for the plugin's event loop thread to exit
    // We don't have direct access to the plugin's thread, so we wait
    // and hope the thread exits within the timeout
    // CRITICAL: Guitarix plugins need at least 1000ms for their event loop to fully exit
    static constexpr int kPluginShutdownTimeoutMs = 1000;
    int effectiveTimeout = timeoutMs > 0 ? timeoutMs : kPluginShutdownTimeoutMs;
    LOGI("gracefulShutdown: Phase 2 - waiting %dms for plugin thread to exit", effectiveTimeout);
    usleep(effectiveTimeout * 1000);  // Convert ms to microseconds
    LOGI("gracefulShutdown: wait complete");
    
    LOGI("gracefulShutdown: EXIT (graceful)");
    return true;
}

} // namespace guitarrackcraft
