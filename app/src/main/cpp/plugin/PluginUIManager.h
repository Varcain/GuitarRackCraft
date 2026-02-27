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

#include <memory>
#include <mutex>
#include <atomic>
#include <string>
#include <vector>
#include "lv2/LV2PluginUI.h"

namespace guitarrackcraft {

class PluginChain;

class PluginUIManager {
public:
    PluginUIManager() = default;
    ~PluginUIManager() = default;

    void setChain(PluginChain* chain) { chain_ = chain; }

    bool createPluginUI(int pluginIndex, int displayNumber,
                        unsigned long parentWindowId,
                        const std::string& nativeLibDir = std::string(),
                        const std::string& x11LibsDir = std::string());

    void destroyPluginUI(int pluginIndex);

    bool idleAllUIs();

    /** Pause all UI idle callbacks (e.g. before reorder). */
    void pauseAllUIs();

    /** Resume all UI idle callbacks (e.g. after reorder). */
    void resumeAllUIs();

    /** Reorder UI entries to match a chain reorder and update captured indices. */
    void reorderUIs(int fromIndex, int toIndex);

    /** Notify the X11 UI (if any) about a parameter change.
     *  Call from UI/JNI thread only. */
    void notifyUIParameterChange(int pluginIndex, uint32_t portIndex, float value);

    /** Get UI entry for a plugin (for checking if UI exists). */
    bool hasUIForPlugin(int pluginIndex) const;

    /** Result of polling for a pending file request from any plugin UI. */
    struct FileRequest {
        int pluginIndex;
        std::string propertyUri;
    };

    /** Poll all UIs for a pending file request (ui:requestValue).
     *  Returns true if a request was found (written to 'out'). */
    bool pollFileRequest(FileRequest& out);

    /** Deliver a file path to a plugin UI via patch:Set atom.
     *  Posts the delivery to the X11 display thread. */
    void deliverFileToUI(int pluginIndex, const std::string& propertyUri, const std::string& filePath);

    /** Clear the plugin pointer in the UI for this index.
     *  Call BEFORE removing the plugin from the chain so
     *  the idle callback won't access a dangling pointer. */
    void detachPlugin(int pluginIndex);

    /** Detach the UI whose captured chain index matches chainIndex,
     *  then decrement all captured indices above it to stay in sync
     *  with the chain's erase. Call BEFORE chain.removePlugin. */
    void detachAndShiftForRemoval(int chainIndex);

private:
    struct UIEntry {
        std::unique_ptr<LV2PluginUI> ui;
        int displayNumber = -1;
        std::shared_ptr<std::atomic<int>> pluginIndex; // mutable index read by closures
        std::shared_ptr<std::atomic<bool>> detached;   // set true after detachPlugin()
    };

    PluginChain* chain_ = nullptr;
    std::vector<UIEntry> uiEntries_;
    mutable std::mutex uiMutex_;
    std::atomic<bool> paused_{false};
};

} // namespace guitarrackcraft
