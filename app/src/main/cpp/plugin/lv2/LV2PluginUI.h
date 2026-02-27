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

#ifndef GUITARRACKCRAFT_LV2_PLUGIN_UI_H
#define GUITARRACKCRAFT_LV2_PLUGIN_UI_H

#include "../IPlugin.h"
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <cstdint>
#include <vector>
#include <unordered_map>

namespace guitarrackcraft {

/**
 * Wrapper for LV2 plugin UIs (X11UI type).
 *
 * Loads the UI shared library via dlopen, finds lv2ui_descriptor(),
 * instantiates the UI, and bridges parameter changes between the
 * native audio chain and the X11 drawing surface.
 *
 * The host-side X11 server is provided by android-xserver (Java).
 * This class only talks to the UI .so through the LV2 UI C API.
 */
class LV2PluginUI {
public:
    using ParameterCallback = std::function<void(uint32_t portIndex, float value)>;
    using AtomCallback = std::function<void(uint32_t portIndex, uint32_t size, const void* data)>;

    LV2PluginUI();
    ~LV2PluginUI();

    /** Set callback for atom messages from the UI (DPF state sync, etc.) */
    void setAtomCallback(AtomCallback cb) { atomCb_ = std::move(cb); }

    /**
     * Instantiate the UI.
     * @param uiBinaryPath  Absolute path to UI .so file (e.g. in files dir)
     * @param uiUri         URI of the UI (from TTL)
     * @param pluginUri     URI of the DSP plugin
     * @param displayNumber X11 display number (e.g. 10 for :10)
     * @param parentWindowId X11 window id supplied by the embedded X server
     * @param plugin        Pointer to the DSP plugin (for initial port values)
     * @param paramCallback Called when the UI writes to a control port
     * @param nativeLibDir  If non-empty and writable, copy uiBinaryPath here and dlopen from there so _binary_pedal_png_* resolve from same namespace
     * @param x11LibsDir    If non-empty, copy libX11.so.6 etc. here into bundle dir before dlopen so plugin DT_NEEDED resolve
     * @return true on success
     */
    bool instantiate(
        const std::string& uiBinaryPath,
        const std::string& uiUri,
        const std::string& pluginUri,
        int displayNumber,
        unsigned long parentWindowId,
        IPlugin* plugin,
        ParameterCallback paramCallback,
        const std::string& nativeLibDir = std::string(),
        const std::string& x11LibsDir = std::string()
    );

    /** Clean up and release the UI instance. */
    void cleanup();

    /**
     * Graceful shutdown - tell plugin to stop its event loop before cleanup.
     * Phase 1: Signal the plugin to exit (destroy window, send message, etc.)
     * Phase 2: Wait for plugin thread to exit
     * @param displayNumber X11 display number for sending signals
     * @param timeoutMs Maximum time to wait for plugin thread to exit (0 = no wait)
     * @return true if graceful shutdown succeeded, false if had to force cleanup
     */
    bool gracefulShutdown(int displayNumber, int timeoutMs = 2000);

    /**
     * Pump the UI's idle interface.
     * Must be called periodically from a non-audio thread.
     * @return 0 on success, non-zero if the UI wants to close
     */
    int idle();

    /** Notify the UI about a port value change. */
    void portEvent(uint32_t portIndex, float value);

    /** Forward an atom event from a DSP output port to the UI.
     *  @param portIndex LV2 port index of the atom output port
     *  @param size      Size of the atom data (LV2_Atom header + body)
     *  @param data      Raw atom data */
    void portEventAtom(uint32_t portIndex, uint32_t size, const void* data);

    /** Poll output control ports and push changed values to the UI.
     *  @param chainMutex If non-null, takes a shared_lock to prevent reorder during reads. */
    void syncOutputPorts(std::shared_mutex* chainMutex = nullptr);

    /** Clear the plugin pointer so idle/syncOutputPorts won't access it.
     *  Call before destroying the plugin object. */
    void clearPlugin() { plugin_.store(nullptr, std::memory_order_release); }

    /** True when a UI handle has been successfully created and no error occurred. */
    bool isValid() const;

    /** Trigger the plugin's resize handler. The plugin will call XGetWindowAttributes
     *  to discover the new size from the X11 server. */
    void triggerResize();

    /** Get and clear pending file request URI (thread-safe).
     *  Returns empty string if no request is pending. */
    std::string getPendingFileRequest();

    /** Set a pending file request URI (called from requestValue callback). */
    void setPendingFileRequest(const std::string& uri);

    /** Deliver a file path to the UI via patch:Set atom port_event.
     *  @param propertyUri The LV2 property URI (e.g. "http://aidadsp.cc/...#json")
     *  @param filePath Absolute path to the file on device */
    void deliverFilePath(const std::string& propertyUri, const std::string& filePath);

    // Forward-declared; defined in LV2PluginUI.cpp
    struct LV2UIDescriptor;

private:

    void* libHandle_ = nullptr;        // dlopen handle
    std::string libCopyPath_;          // if non-empty, we copied the .so here; unlink in cleanup
    void* uiHandle_  = nullptr;        // LV2UI_Handle
    const LV2UIDescriptor* desc_ = nullptr;

    ParameterCallback paramCb_;
    AtomCallback atomCb_;
    std::atomic<IPlugin*> plugin_{nullptr};

    // ui:portMap data — maps port symbols to LV2 port indices (for DPF bypass inversion)
    std::unordered_map<std::string, uint32_t> portSymbolMap_;
    struct PortMapData {
        void* handle;
        uint32_t (*port_index)(void* handle, const char* symbol);
    };
    PortMapData portMapData_{};

    // Cached idle function pointer (extension_data result)
    int (*idleFn_)(void* handle) = nullptr;
    // Cached resize function pointer (extension_data result)
    int (*resizeFn_)(void* handle, int w, int h) = nullptr;

    // Output control port tracking for syncOutputPorts()
    std::vector<uint32_t> outputControlPorts_;         // LV2 port indices of control outputs
    std::unordered_map<uint32_t, float> lastOutputValues_;  // last sent value per port index

    // Heap-allocated LV2UI_Request_Value data (must outlive instantiate() since
    // the plugin stores a pointer to it via the LV2 feature array)
    void* requestValueData_ = nullptr;

    // Pending file request from native UI (ui:requestValue)
    std::mutex fileRequestMutex_;
    std::string pendingFileRequestUri_;

    // Static LV2 UI write-function callback
    static void writeFunction(void* controller,
                              uint32_t portIndex,
                              uint32_t bufferSize,
                              uint32_t portProtocol,
                              const void* buffer);
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_LV2_PLUGIN_UI_H
