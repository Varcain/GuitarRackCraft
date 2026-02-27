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

#ifndef GUITARRACKCRAFT_LV2_PLUGIN_H
#define GUITARRACKCRAFT_LV2_PLUGIN_H

#include "../IPlugin.h"
#include <string>
#include <vector>
#include <memory>
#include <atomic>

#if defined(HAVE_LV2) && HAVE_LV2 == 1
#include <lilv/lilv.h>
#include <lv2/worker/worker.h>
#include <lv2/options/options.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/patch/patch.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/state/state.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#else
struct LilvInstance_;
struct LilvPlugin_;
struct LilvWorld_;
#endif

namespace guitarrackcraft {

/**
 * LV2 plugin wrapper implementing IPlugin interface.
 * Wraps LilvInstance and handles LV2-specific audio processing.
 */
class LV2Plugin : public IPlugin {
public:
#if defined(HAVE_LV2) && HAVE_LV2 == 1
    LV2Plugin(const LilvPlugin* plugin, LilvWorld* world, float sampleRate,
              const std::string& filesDir = std::string());
#else
    LV2Plugin(LilvPlugin_* plugin, LilvWorld_* world, float sampleRate,
              const std::string& filesDir = std::string());
#endif
    ~LV2Plugin() override;

    // IPlugin interface
    void activate(float sampleRate, uint32_t bufferSize = 0) override;
    void deactivate() override;
    void process(const float* const* inputs, float* const* outputs, uint32_t numFrames) override;
    PluginInfo getInfo() const override;
    void setParameter(uint32_t portIndex, float value) override;
    float getParameter(uint32_t portIndex) const override;
    uint32_t getNumInputPorts() const override;
    uint32_t getNumOutputPorts() const override;

    /** True if the plugin binary loaded and instantiated successfully. */
    bool hasInstance() const { return instance_ != nullptr; }

    /** Send a file path to the plugin via LV2 patch:Set atom message. */
    void setFilePath(const std::string& propertyUri, const std::string& path) override;

    /** Inject a raw atom message into the plugin's atom input port (for UI→DSP forwarding). */
    void injectAtom(const void* data, uint32_t size) override;

    /** Drain queued atom events from output ports. */
    std::vector<OutputAtomEvent> drainOutputAtoms() override;

    /** Save complete plugin state (control ports + LV2 state:interface properties). */
    PluginState saveState() override;

    /** Restore plugin state. Must be called with exclusive lock (no concurrent process()). */
    bool restoreState(const PluginState& state) override;

private:
#if defined(HAVE_LV2) && HAVE_LV2 == 1
    const LilvPlugin* plugin_;
    LilvWorld* world_;
    LilvInstance* instance_;
#else
    LilvPlugin_* plugin_;
    LilvWorld_* world_;
    LilvInstance_* instance_;
#endif
    float sampleRate_;
    std::atomic<bool> isActive_{false};
    std::atomic<bool> processing_{false}; // guards instance_ use in process()

    std::vector<std::unique_ptr<float>> controlPorts_;
    /** Global LV2 port index for each control port (same order as controlPorts_). */
    std::vector<uint32_t> controlPortIndices_;
    std::vector<std::vector<float>> audioInputBuffers_;
    std::vector<std::vector<float>> audioOutputBuffers_;
    std::vector<float*> audioInputPorts_;
    std::vector<float*> audioOutputPorts_;

    static constexpr size_t kMaxLv2BufferFrames = 8192;

    void connectPorts();
    void initializePorts();

#if defined(HAVE_LV2) && HAVE_LV2 == 1
    // Worker extension
    const LV2_Worker_Interface* workerInterface_ = nullptr;
    LV2_Worker_Schedule workerSchedule_{};
    LV2_Feature workerScheduleFeature_{};
    LV2_Feature optionsFeature_{};
    LV2_Feature boundedBlockFeature_{};
    LV2_Options_Option options_[4]{};
    int32_t maxBlockLength_ = kMaxLv2BufferFrames;
    std::vector<const LV2_Feature*> instanceFeatures_;

    std::thread workerThread_;
    std::mutex workerMutex_;
    std::condition_variable workerCond_;
    std::queue<std::vector<uint8_t>> workRequests_;
    std::mutex responseMutex_;
    std::queue<std::vector<uint8_t>> workResponses_;
    std::atomic<bool> workerRunning_{false};

    // Atom port buffers
    static constexpr size_t kAtomBufferSize = 8192;
    struct AtomPortInfo {
        uint32_t portIndex;
        bool isInput;
        size_t bufferIdx;
    };
    std::vector<std::vector<uint8_t>> atomPortBuffers_;
    std::vector<AtomPortInfo> atomPorts_;

    // Atom forge + patch:Set support
    LV2_Atom_Forge forge_{};
    LV2_URID atom_Path_ = 0;
    LV2_URID atom_URID_ = 0;
    LV2_URID patch_Set_ = 0;
    LV2_URID patch_Get_ = 0;
    LV2_URID patch_property_ = 0;
    LV2_URID patch_value_ = 0;

    // Pending file path for patch:Set delivery
    std::string pendingFilePath_;
    std::string pendingFilePropertyUri_;
    std::mutex filePathMutex_;

    // Pending raw atom messages from UI (DPF state sync, etc.)
    std::mutex pendingAtomMutex_;
    std::vector<std::vector<uint8_t>> pendingAtoms_;

    // Queued atom output events for UI forwarding (written by process, read by UI thread)
    std::mutex outputAtomMutex_;
    std::vector<OutputAtomEvent> pendingOutputAtoms_;

    // State extension (save/restore)
    const LV2_State_Interface* stateInterface_ = nullptr;
    LV2_State_Map_Path mapPathData_{};
    LV2_State_Free_Path freePathData_{};
    LV2_Feature mapPathFeature_{};
    LV2_Feature freePathFeature_{};
    std::string filesDir_;

    static char* mapAbstractPathCallback(LV2_State_Map_Path_Handle handle, const char* absolutePath);
    static char* mapAbsolutePathCallback(LV2_State_Map_Path_Handle handle, const char* abstractPath);
    static void freePathCallback(LV2_State_Free_Path_Handle handle, char* path);

    void buildFeatures();
    void startWorker();
    void stopWorker();
    void workerThreadFunc();
    static LV2_Worker_Status scheduleWorkCallback(
        LV2_Worker_Schedule_Handle handle, uint32_t size, const void* data);
    static LV2_Worker_Status respondCallback(
        LV2_Worker_Respond_Handle handle, uint32_t size, const void* data);
#endif
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_LV2_PLUGIN_H
