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

#include "LV2Plugin.h"
#include "LV2Utils.h"
#include "../PluginUIGuard.h"
#include <android/log.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

#define LOG_TAG "LV2Plugin"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#if defined(HAVE_LV2) && HAVE_LV2 == 1
#include <lilv/lilv.h>
#include <lv2/urid/urid.h>
#include <lv2/atom/util.h>

// ---------- Global URID map (shared across all plugin instances + UIs) ------

namespace {

struct UridMapImpl {
    std::mutex mutex;
    std::unordered_map<std::string, LV2_URID> uriToId;
    std::vector<std::string> idToUri;  // index 0 unused (URID 0 is invalid)

    UridMapImpl() { idToUri.emplace_back(""); } // reserve index 0

    LV2_URID map(const char* uri) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = uriToId.find(uri);
        if (it != uriToId.end()) return it->second;
        LV2_URID id = static_cast<LV2_URID>(idToUri.size());
        uriToId[uri] = id;
        idToUri.push_back(uri);
        return id;
    }

    const char* unmap(LV2_URID id) {
        std::lock_guard<std::mutex> lock(mutex);
        if (id == 0 || id >= idToUri.size()) return nullptr;
        return idToUri[id].c_str();
    }
};

UridMapImpl& getGlobalUridMap() {
    static UridMapImpl instance;
    return instance;
}

LV2_URID uridMapCallback(LV2_URID_Map_Handle handle, const char* uri) {
    return static_cast<UridMapImpl*>(handle)->map(uri);
}

const char* uridUnmapCallback(LV2_URID_Unmap_Handle handle, LV2_URID id) {
    return static_cast<UridMapImpl*>(handle)->unmap(id);
}

} // anonymous namespace

// Exposed so LV2PluginUI.cpp can share the same URID numbering (UI↔DSP).
LV2_URID_Map globalLv2UridMap = { &getGlobalUridMap(), uridMapCallback };
LV2_URID_Unmap globalLv2UridUnmap = { &getGlobalUridMap(), uridUnmapCallback };

namespace {
LV2_Feature uridMapFeature = { LV2_URID__map, &globalLv2UridMap };
LV2_Feature uridUnmapFeature = { LV2_URID__unmap, &globalLv2UridUnmap };
} // anonymous namespace
#endif

namespace guitarrackcraft {

#if defined(HAVE_LV2) && HAVE_LV2 == 1

// Check if we support all required features of a plugin
static bool checkRequiredFeatures(const LilvPlugin* plugin, LilvWorld* world) {
    static const char* supportedFeatures[] = {
        LV2_URID__map,
        LV2_URID__unmap,
        LV2_WORKER__schedule,
        LV2_OPTIONS__options,
        LV2_BUF_SIZE__boundedBlockLength,
        LV2_STATE__mapPath,
        LV2_STATE__freePath,
        nullptr
    };

    LilvNodes* required = lilv_plugin_get_required_features(plugin);
    if (!required) return true;

    bool ok = true;
    LILV_FOREACH(nodes, i, required) {
        const LilvNode* feat = lilv_nodes_get(required, i);
        const char* uri = lilv_node_as_uri(feat);
        bool found = false;
        for (const char** s = supportedFeatures; *s; ++s) {
            if (strcmp(uri, *s) == 0) { found = true; break; }
        }
        if (!found) {
            LOGE("Plugin requires unsupported feature: %s", uri);
            ok = false;
        }
    }
    lilv_nodes_free(required);
    return ok;
}

void LV2Plugin::buildFeatures() {
    auto& uridMap = getGlobalUridMap();

    // Initialize atom forge
    lv2_atom_forge_init(&forge_, &globalLv2UridMap);

    // Map patch/atom URIDs
    atom_Path_ = uridMap.map(LV2_ATOM__Path);
    atom_URID_ = uridMap.map(LV2_ATOM__URID);
    patch_Set_ = uridMap.map(LV2_PATCH__Set);
    patch_Get_ = uridMap.map(LV2_PATCH__Get);
    patch_property_ = uridMap.map(LV2_PATCH__property);
    patch_value_ = uridMap.map(LV2_PATCH__value);

    // Options: provide buffer size info
    LV2_URID bufsz_max = uridMap.map(LV2_BUF_SIZE__maxBlockLength);
    LV2_URID bufsz_nom = uridMap.map(LV2_BUF_SIZE__nominalBlockLength);
    LV2_URID atom_Int = uridMap.map(LV2_ATOM__Int);

    // maxBlockLength_ is set by activate() — use actual callback buffer size when available.
    // Provide both maxBlockLength and nominalBlockLength (same value) because some plugins
    // (e.g. GxCabinet) prefer nominalBlockLength for convolver quantum configuration.
    options_[0] = {LV2_OPTIONS_INSTANCE, 0, bufsz_max, sizeof(int32_t), atom_Int, &maxBlockLength_};
    options_[1] = {LV2_OPTIONS_INSTANCE, 0, bufsz_nom, sizeof(int32_t), atom_Int, &maxBlockLength_};
    options_[2] = {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr}; // terminator

    // Worker schedule (handle = this)
    workerSchedule_ = {this, scheduleWorkCallback};

    // Per-instance feature structs
    workerScheduleFeature_ = {LV2_WORKER__schedule, &workerSchedule_};
    optionsFeature_ = {LV2_OPTIONS__options, options_};
    boundedBlockFeature_ = {LV2_BUF_SIZE__boundedBlockLength, nullptr};

    // State path mapping features
    mapPathData_.handle = this;
    mapPathData_.abstract_path = mapAbstractPathCallback;
    mapPathData_.absolute_path = mapAbsolutePathCallback;
    mapPathFeature_ = {LV2_STATE__mapPath, &mapPathData_};

    freePathData_.handle = this;
    freePathData_.free_path = freePathCallback;
    freePathFeature_ = {LV2_STATE__freePath, &freePathData_};

    // Build null-terminated features pointer array
    instanceFeatures_.clear();
    instanceFeatures_.push_back(&uridMapFeature);         // global
    instanceFeatures_.push_back(&uridUnmapFeature);       // global
    instanceFeatures_.push_back(&workerScheduleFeature_);  // per-instance
    instanceFeatures_.push_back(&optionsFeature_);         // per-instance
    instanceFeatures_.push_back(&boundedBlockFeature_);    // static
    instanceFeatures_.push_back(&mapPathFeature_);         // per-instance
    instanceFeatures_.push_back(&freePathFeature_);        // per-instance
    instanceFeatures_.push_back(nullptr);
}

LV2Plugin::LV2Plugin(const LilvPlugin* plugin, LilvWorld* world, float sampleRate, const std::string& filesDir)
    : plugin_(plugin)
    , world_(world)
    , instance_(nullptr)
    , sampleRate_(sampleRate)
    , isActive_(false)
    , filesDir_(filesDir)
{
    if (!plugin_ || !world_) {
        LOGE("Invalid plugin or world");
        return;
    }

    if (!checkRequiredFeatures(plugin_, world_)) {
        LOGE("Plugin has unsupported required features, skipping instantiation");
        return;
    }

    buildFeatures();

    instance_ = lilv_plugin_instantiate(plugin_, sampleRate, instanceFeatures_.data());
    if (!instance_) {
        LOGE("Failed to instantiate LV2 plugin");
        return;
    }

    initializePorts();
    connectPorts();

    // Query state:interface extension
    const void* si = lilv_instance_get_extension_data(instance_, LV2_STATE__interface);
    stateInterface_ = static_cast<const LV2_State_Interface*>(si);
    if (stateInterface_) {
        LOGI("Plugin provides state:interface (save=%p restore=%p)",
             (void*)stateInterface_->save, (void*)stateInterface_->restore);
    }
}

LV2Plugin::~LV2Plugin() {
    stopWorker();
    deactivate();
    if (instance_) {
        if (!guitarrackcraft::isCreatingPluginUI()) {
            lilv_instance_free(instance_);
        }
        instance_ = nullptr;
    }
}

void LV2Plugin::activate(float sampleRate, uint32_t bufferSize) {
    LOGI("activate: sampleRate=%.0f bufferSize=%u", sampleRate, bufferSize);
    if (isActive_.load(std::memory_order_seq_cst) && sampleRate_ == sampleRate) {
        return;
    }

    stopWorker();
    deactivate();

    sampleRate_ = sampleRate;
    // Use actual callback buffer size for maxBlockLength so convolver plugins
    // configure their partition size correctly. Round up to next power of 2
    // because zita-convolver requires power-of-2 quantum. Fall back to
    // kMaxLv2BufferFrames only if no buffer size is provided.
    if (bufferSize > 0) {
        // Round up to next power of 2
        uint32_t po2 = 1;
        while (po2 < bufferSize) po2 <<= 1;
        maxBlockLength_ = static_cast<int32_t>(po2);
        LOGI("activate: framesPerBurst=%u rounded to power-of-2 maxBlockLength=%d",
             bufferSize, maxBlockLength_);
    } else {
        maxBlockLength_ = static_cast<int32_t>(kMaxLv2BufferFrames);
    }

    // Save state before destroying the old instance so it can be restored after
    // re-instantiation (e.g. NAM model path is held inside the plugin instance).
    PluginState savedState;
    bool hasSavedState = false;
    if (instance_) {
        savedState = saveState();
        hasSavedState = !savedState.properties.empty() || !savedState.controlPortValues.empty();
        // Mark inactive BEFORE waiting (Dekker's pattern with process()).
        isActive_.store(false, std::memory_order_seq_cst);
        // Wait for any in-flight process() call to finish using instance_.
        while (processing_.load(std::memory_order_seq_cst)) {
            std::this_thread::yield();
        }
        stopWorker();
        lilv_instance_free(instance_);
        instance_ = nullptr;
    }

    if (!checkRequiredFeatures(plugin_, world_)) {
        LOGE("Plugin has unsupported required features, skipping re-instantiation");
        return;
    }

    buildFeatures();

    instance_ = lilv_plugin_instantiate(plugin_, sampleRate, instanceFeatures_.data());
    if (!instance_) {
        LOGE("Failed to re-instantiate LV2 plugin");
        return;
    }

    initializePorts();
    connectPorts();

    // Re-query state:interface after re-instantiation
    const void* si = lilv_instance_get_extension_data(instance_, LV2_STATE__interface);
    stateInterface_ = static_cast<const LV2_State_Interface*>(si);

    LOGI("activate: ports control=%zu audioIn=%zu audioOut=%zu atom=%zu",
         controlPorts_.size(), audioInputPorts_.size(), audioOutputPorts_.size(),
         atomPorts_.size());

    if (instance_) {
        lilv_instance_activate(instance_);
        startWorker();
    }

    isActive_.store(true, std::memory_order_seq_cst);

    // Restore state after re-instantiation (e.g. NAM model path).
    // The worker thread is already running so the plugin can immediately
    // schedule async work (model load) in response to restore().
    if (hasSavedState && instance_) {
        restoreState(savedState);
    }
}

void LV2Plugin::deactivate() {
    if (!isActive_.load(std::memory_order_seq_cst)) {
        return;
    }

    isActive_.store(false, std::memory_order_seq_cst);
    // Wait for any in-flight process() call to finish using instance_.
    while (processing_.load(std::memory_order_seq_cst)) {
        std::this_thread::yield();
    }

    stopWorker();

    if (instance_) {
        lilv_instance_deactivate(instance_);
    }
}

void LV2Plugin::process(const float* const* inputs, float* const* outputs, uint32_t numFrames) {
    // Signal that we're about to use instance_. Must be set BEFORE reading
    // isActive_ so that activate()'s spin-wait cannot miss an in-flight call
    // (Dekker's pattern: each side stores its own flag, then reads the other).
    processing_.store(true, std::memory_order_seq_cst);

    if (!isActive_.load(std::memory_order_seq_cst) || !instance_) {
        processing_.store(false, std::memory_order_seq_cst);
        static auto lastLogPassthrough = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastLogPassthrough).count() >= 1.0) {
            lastLogPassthrough = now;
            LOGI("process: passthrough isActive=%d instance=%p", (int)isActive_.load(), (void*)instance_);
        }
        if (inputs && outputs && numFrames > 0) {
            for (uint32_t ch = 0; ch < 2; ++ch) {
                if (inputs[ch] && outputs[ch]) {
                    std::memcpy(outputs[ch], inputs[ch], numFrames * sizeof(float));
                }
            }
        }
        return;
    }

    // Copy host input into the instance's connected buffers
    const size_t maxCopy = std::min(static_cast<size_t>(numFrames), kMaxLv2BufferFrames);
    for (size_t i = 0; i < audioInputPorts_.size() && i < 2; ++i) {
        if (inputs[i] && audioInputPorts_[i]) {
            std::memcpy(audioInputPorts_[i], inputs[i], maxCopy * sizeof(float));
        }
    }

    // Reset atom buffers before run()
    for (auto& ap : atomPorts_) {
        auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(
            atomPortBuffers_[ap.bufferIdx].data());
        seq->atom.type = getGlobalUridMap().map(LV2_ATOM__Sequence);
        if (ap.isInput) {
            // Input: empty sequence (body header only)
            seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
        } else {
            // Output: set atom.size to total buffer capacity so the plugin's
            // forge knows how much space is available for writing
            seq->atom.size = kAtomBufferSize;
        }
        seq->body.unit = 0;
        seq->body.pad = 0;
    }

    // Inject pending raw atom messages from UI (DPF state sync, etc.)
    {
        std::lock_guard<std::mutex> lock(pendingAtomMutex_);
        if (!pendingAtoms_.empty()) {
            for (auto& ap : atomPorts_) {
                if (ap.isInput) {
                    auto* buf = atomPortBuffers_[ap.bufferIdx].data();
                    auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(buf);
                    // Append each pending atom as an event in the sequence
                    for (auto& atomData : pendingAtoms_) {
                        if (atomData.size() < sizeof(LV2_Atom)) continue;
                        auto* srcAtom = reinterpret_cast<const LV2_Atom*>(atomData.data());
                        uint32_t bodyDataSize = srcAtom->size;
                        // Event: time(8) + atom header(8) + body data
                        uint32_t evtBytes = sizeof(LV2_Atom_Event) + bodyDataSize;
                        uint32_t paddedEvt = (evtBytes + 7u) & ~7u;
                        // Check if there's room in the buffer
                        uint32_t totalUsed = sizeof(LV2_Atom) + seq->atom.size;
                        if (totalUsed + paddedEvt > kAtomBufferSize) continue;
                        // Write at end of current sequence
                        auto* writePos = reinterpret_cast<uint8_t*>(&seq->body) + seq->atom.size;
                        auto* evt = reinterpret_cast<LV2_Atom_Event*>(writePos);
                        evt->time.frames = 0;
                        evt->body.size = bodyDataSize;
                        evt->body.type = srcAtom->type;
                        std::memcpy(reinterpret_cast<uint8_t*>(evt) + sizeof(LV2_Atom_Event),
                                    atomData.data() + sizeof(LV2_Atom),
                                    bodyDataSize);
                        seq->atom.size += paddedEvt;
                    }
                    break;  // only inject into first atom input port
                }
            }
            pendingAtoms_.clear();
        }
    }

    // Forge pending patch:Set message into atom input port
    {
        std::lock_guard<std::mutex> lock(filePathMutex_);
        if (!pendingFilePath_.empty()) {
            for (auto& ap : atomPorts_) {
                if (ap.isInput) {
                    auto* buf = atomPortBuffers_[ap.bufferIdx].data();
                    lv2_atom_forge_set_buffer(&forge_, buf, kAtomBufferSize);

                    LV2_Atom_Forge_Frame seqFrame;
                    lv2_atom_forge_sequence_head(&forge_, &seqFrame, 0);

                    LV2_Atom_Forge_Frame objFrame;
                    lv2_atom_forge_frame_time(&forge_, 0);
                    lv2_atom_forge_object(&forge_, &objFrame, 0, patch_Set_);

                    lv2_atom_forge_key(&forge_, patch_property_);
                    LV2_URID propUrid = getGlobalUridMap().map(
                        pendingFilePropertyUri_.c_str());
                    lv2_atom_forge_urid(&forge_, propUrid);

                    lv2_atom_forge_key(&forge_, patch_value_);
                    lv2_atom_forge_path(&forge_, pendingFilePath_.c_str(),
                                        pendingFilePath_.size() + 1);

                    lv2_atom_forge_pop(&forge_, &objFrame);
                    lv2_atom_forge_pop(&forge_, &seqFrame);

                    LOGI("process: forged patch:Set for %s -> %s",
                         pendingFilePropertyUri_.c_str(),
                         pendingFilePath_.c_str());
                    break;
                }
            }
            pendingFilePath_.clear();
            pendingFilePropertyUri_.clear();
        }
    }

    // Re-check instance_ (defensive — processing_ guard should prevent this)
    if (!instance_) {
        processing_.store(false, std::memory_order_seq_cst);
        for (uint32_t ch = 0; ch < 2; ++ch) {
            if (inputs[ch] && outputs[ch])
                std::memcpy(outputs[ch], inputs[ch], maxCopy * sizeof(float));
        }
        return;
    }
    lilv_instance_run(instance_, static_cast<uint32_t>(maxCopy));

    // Call end_run if available
    if (workerInterface_ && workerInterface_->end_run) {
        workerInterface_->end_run(lilv_instance_get_handle(instance_));
    }

    // Deliver pending worker responses AFTER run() (LV2 Worker spec requirement).
    // Plugins like AIDA-X write to the atom forge in work_response(), which
    // requires the forge to be initialized by run() first. Delivering before
    // run() used stale forge state and violated the spec ordering.
    if (workerInterface_ && workerInterface_->work_response) {
        LV2_Handle handle = lilv_instance_get_handle(instance_);
        std::lock_guard<std::mutex> lock(responseMutex_);
        while (!workResponses_.empty()) {
            auto& resp = workResponses_.front();
            workerInterface_->work_response(
                handle,
                static_cast<uint32_t>(resp.size()),
                resp.data());
            workResponses_.pop();
        }
    }

    // Read atom output ports and queue events for UI forwarding
    {
        std::vector<OutputAtomEvent> batch;
        for (auto& ap : atomPorts_) {
            if (ap.isInput) continue;
            auto* seq = reinterpret_cast<const LV2_Atom_Sequence*>(
                atomPortBuffers_[ap.bufferIdx].data());
            // Skip if empty or if the plugin left atom.size at the pre-run
            // capacity (meaning it didn't write the output sequence at all —
            // iterating would walk through uninitialised memory).
            if (seq->atom.size <= sizeof(LV2_Atom_Sequence_Body) ||
                seq->atom.size >= kAtomBufferSize) continue;
            LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
                uint32_t bodySize = ev->body.size;
                // Sanity-check: event body must fit inside the atom buffer
                const uint8_t* evEnd = reinterpret_cast<const uint8_t*>(&ev->body) +
                                       sizeof(LV2_Atom) + bodySize;
                const uint8_t* bufEnd = atomPortBuffers_[ap.bufferIdx].data() +
                                        kAtomBufferSize;
                if (evEnd > bufEnd) break;
                uint32_t atomTotalSize = sizeof(LV2_Atom) + bodySize;
                std::vector<uint8_t> data(atomTotalSize);
                std::memcpy(data.data(), &ev->body, atomTotalSize);
                batch.push_back(OutputAtomEvent{ap.portIndex, std::move(data)});
            }
        }
        if (!batch.empty()) {
            std::lock_guard<std::mutex> lock(outputAtomMutex_);
            for (auto& evt : batch) {
                pendingOutputAtoms_.push_back(std::move(evt));
            }
        }
    }

    const size_t numOut = audioOutputPorts_.size();
    for (size_t i = 0; i < numOut && i < 2; ++i) {
        if (outputs[i] && audioOutputPorts_[i]) {
            std::memcpy(outputs[i], audioOutputPorts_[i], maxCopy * sizeof(float));
        }
    }
    // Mono plugin: duplicate single output to both channels
    if (numOut == 1 && outputs[0] && outputs[1] && audioOutputPorts_[0]) {
        std::memcpy(outputs[1], audioOutputPorts_[0], maxCopy * sizeof(float));
    }
    // If host requested more frames than our internal buffer, passthrough the tail
    if (maxCopy < static_cast<size_t>(numFrames)) {
        for (uint32_t ch = 0; ch < 2; ++ch) {
            if (inputs[ch] && outputs[ch]) {
                std::memcpy(outputs[ch] + maxCopy, inputs[ch] + maxCopy,
                            (numFrames - maxCopy) * sizeof(float));
            }
        }
    }

    // Rate-limited: in/out peak to verify plugin is changing the signal
    {
        static auto lastLogPeak = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastLogPeak).count() >= 1.0) {
            lastLogPeak = now;
            const uint32_t n = std::min(numFrames, 128u);
            float inPeak = 0.0f, outPeak = 0.0f;
            if (inputs[0]) {
                for (uint32_t i = 0; i < n; ++i) {
                    float s = std::fabs(inputs[0][i]);
                    if (s > inPeak) inPeak = s;
                }
            }
            if (outputs[0]) {
                for (uint32_t i = 0; i < n; ++i) {
                    float s = std::fabs(outputs[0][i]);
                    if (s > outPeak) outPeak = s;
                }
            }
            LOGI("process: inPeak=%.4f outPeak=%.4f nFrames=%u", inPeak, outPeak, numFrames);
        }
    }

    processing_.store(false, std::memory_order_seq_cst);
}

PluginInfo LV2Plugin::getInfo() const {
    PluginInfo info;
    
    if (!plugin_) {
        return info;
    }
    
    const LilvNode* uri = lilv_plugin_get_uri(plugin_);
    const LilvNode* name = lilv_plugin_get_name(plugin_);
    
    if (uri) {
        info.id = lilv_node_as_string(uri);
    }
    if (name) {
        info.name = lilv_node_as_string(name);
    }
    info.format = "LV2";
    
    // Get port information
    uint32_t numPorts = lilv_plugin_get_num_ports(plugin_);
    info.ports.reserve(numPorts);
    
    LilvNode* audioClass = lilv_new_uri(world_, LILV_URI_AUDIO_PORT);
    LilvNode* controlClass = lilv_new_uri(world_, LILV_URI_CONTROL_PORT);
    LilvNode* inputClass = lilv_new_uri(world_, LILV_URI_INPUT_PORT);
    LilvNode* outputClass = lilv_new_uri(world_, LILV_URI_OUTPUT_PORT);
    LilvNode* toggledClass = lilv_new_uri(world_, LV2_CORE__toggled);
    
    for (uint32_t i = 0; i < numPorts; ++i) {
        const LilvPort* port = lilv_plugin_get_port_by_index(plugin_, i);
        if (!port) continue;
        
        PortInfo portInfo;
        portInfo.index = i;
        
        const LilvNode* portName = lilv_port_get_name(plugin_, port);
        const LilvNode* portSymbol = lilv_port_get_symbol(plugin_, port);
        
        if (portName) {
            portInfo.name = lilv_node_as_string(portName);
        }
        if (portSymbol) {
            portInfo.symbol = lilv_node_as_string(portSymbol);
        }
        
        // Check port classes
        bool isAudio = lilv_port_is_a(plugin_, port, audioClass);
        bool isControl = lilv_port_is_a(plugin_, port, controlClass);
        bool isInput = lilv_port_is_a(plugin_, port, inputClass);
        bool isOutput = lilv_port_is_a(plugin_, port, outputClass);
        
        portInfo.isAudio = isAudio;
        portInfo.isControl = isControl;
        portInfo.isInput = isInput;
        portInfo.isToggle = isControl && lilv_port_has_property(plugin_, port, toggledClass);
        
        // Get default, min, max values for control ports
        if (isControl) {
            LilvNode* defNode = nullptr;
            LilvNode* minNode = nullptr;
            LilvNode* maxNode = nullptr;
            lilv_port_get_range(plugin_, port, &defNode, &minNode, &maxNode);
            if (defNode) {
                portInfo.defaultValue = static_cast<float>(lilv_node_as_float(defNode));
                lilv_node_free(defNode);
            }
            if (minNode) {
                portInfo.minValue = static_cast<float>(lilv_node_as_float(minNode));
                lilv_node_free(minNode);
            }
            if (maxNode) {
                portInfo.maxValue = static_cast<float>(lilv_node_as_float(maxNode));
                lilv_node_free(maxNode);
            }
            // Scale points (enumeration values) for dropdown UI
            LilvScalePoints* points = lilv_port_get_scale_points(plugin_, port);
            if (points) {
                LILV_FOREACH(scale_points, it, points) {
                    const LilvScalePoint* sp = lilv_scale_points_get(points, it);
                    if (sp) {
                        ScalePoint spOut;
                        const LilvNode* labelNode = lilv_scale_point_get_label(sp);
                        const LilvNode* valueNode = lilv_scale_point_get_value(sp);
                        if (labelNode) {
                            spOut.label = lilv_node_as_string(labelNode);
                        }
                        if (valueNode) {
                            spOut.value = static_cast<float>(lilv_node_as_float(valueNode));
                        }
                        portInfo.scalePoints.push_back(spOut);
                    }
                }
                lilv_scale_points_free(points);
            }
        }

        info.ports.push_back(portInfo);
    }
    
    lilv_node_free(audioClass);
    lilv_node_free(controlClass);
    lilv_node_free(inputClass);
    lilv_node_free(outputClass);
    lilv_node_free(toggledClass);
    
    // Discover modgui (modgui.ttl + iconTemplate)
    discoverModguiMetadata(plugin_, info);

    // Discover X11 UI
    LilvNode* x11UiClass = lilv_new_uri(world_, "http://lv2plug.in/ns/extensions/ui#X11UI");
    if (x11UiClass) {
        LilvUIs* uis = lilv_plugin_get_uis(plugin_);
        if (uis) {
            LILV_FOREACH(uis, u, uis) {
                const LilvUI* ui = lilv_uis_get(uis, u);
                if (!lilv_ui_is_a(ui, x11UiClass)) continue;
                std::string binaryPath = resolveX11UIBinaryPath(ui, plugin_, world_);
                if (!binaryPath.empty()) {
                    info.hasX11Ui = true;
                    info.x11UiBinaryPath = binaryPath;
                    info.x11UiUri = lilv_node_as_string(lilv_ui_get_uri(ui));
                    break;
                }
            }
            lilv_uis_free(uis);
        }
        lilv_node_free(x11UiClass);
    }
    
    return info;
}

void LV2Plugin::setParameter(uint32_t portIndex, float value) {
    // portIndex is the global LV2 port index; map to control buffer index
    for (size_t k = 0; k < controlPortIndices_.size(); ++k) {
        if (controlPortIndices_[k] == portIndex && k < controlPorts_.size() && controlPorts_[k]) {
            *controlPorts_[k].get() = value;
            return;
        }
    }
}

float LV2Plugin::getParameter(uint32_t portIndex) const {
    for (size_t k = 0; k < controlPortIndices_.size(); ++k) {
        if (controlPortIndices_[k] == portIndex && k < controlPorts_.size() && controlPorts_[k]) {
            return *controlPorts_[k].get();
        }
    }
    return 0.0f;
}

uint32_t LV2Plugin::getNumInputPorts() const {
    return static_cast<uint32_t>(audioInputPorts_.size());
}

uint32_t LV2Plugin::getNumOutputPorts() const {
    return static_cast<uint32_t>(audioOutputPorts_.size());
}

void LV2Plugin::setFilePath(const std::string& propertyUri, const std::string& path) {
    std::lock_guard<std::mutex> lock(filePathMutex_);
    pendingFilePropertyUri_ = propertyUri;
    pendingFilePath_ = path;
    LOGI("setFilePath: property=%s path=%s", propertyUri.c_str(), path.c_str());
}

void LV2Plugin::injectAtom(const void* data, uint32_t size) {
    if (!data || size == 0) return;
    std::vector<uint8_t> buf(size);
    std::memcpy(buf.data(), data, size);
    {
        std::lock_guard<std::mutex> lock(pendingAtomMutex_);
        pendingAtoms_.push_back(std::move(buf));
    }
    LOGI("injectAtom: queued %u bytes for atom input port", size);
}

std::vector<OutputAtomEvent> LV2Plugin::drainOutputAtoms() {
    std::lock_guard<std::mutex> lock(outputAtomMutex_);
    std::vector<OutputAtomEvent> result;
    result.swap(pendingOutputAtoms_);
    return result;
}

void LV2Plugin::connectPorts() {
    if (!instance_ || !plugin_) {
        return;
    }

    uint32_t controlIdx = 0;
    uint32_t audioInputIdx = 0;
    uint32_t audioOutputIdx = 0;

    uint32_t numPorts = lilv_plugin_get_num_ports(plugin_);
    LilvNode* audioClass = lilv_new_uri(world_, LILV_URI_AUDIO_PORT);
    LilvNode* controlClass = lilv_new_uri(world_, LILV_URI_CONTROL_PORT);
    LilvNode* inputClass = lilv_new_uri(world_, LILV_URI_INPUT_PORT);

    for (uint32_t i = 0; i < numPorts; ++i) {
        const LilvPort* port = lilv_plugin_get_port_by_index(plugin_, i);
        if (!port) continue;

        bool isAudio = lilv_port_is_a(plugin_, port, audioClass);
        bool isControl = lilv_port_is_a(plugin_, port, controlClass);
        bool isInput = lilv_port_is_a(plugin_, port, inputClass);

        // Check if this is an atom port we have a buffer for
        bool connectedAtom = false;
        for (auto& ap : atomPorts_) {
            if (ap.portIndex == i) {
                lilv_instance_connect_port(instance_, i,
                    atomPortBuffers_[ap.bufferIdx].data());
                connectedAtom = true;
                break;
            }
        }
        if (connectedAtom) continue;

        if (isControl && controlIdx < controlPorts_.size()) {
            lilv_instance_connect_port(instance_, i, controlPorts_[controlIdx].get());
            controlIdx++;
        } else if (isAudio && isInput && audioInputIdx < audioInputPorts_.size()) {
            lilv_instance_connect_port(instance_, i, audioInputPorts_[audioInputIdx]);
            audioInputIdx++;
        } else if (isAudio && !isInput && audioOutputIdx < audioOutputPorts_.size()) {
            lilv_instance_connect_port(instance_, i, audioOutputPorts_[audioOutputIdx]);
            audioOutputIdx++;
        } else {
            lilv_instance_connect_port(instance_, i, nullptr);
        }
    }

    lilv_node_free(audioClass);
    lilv_node_free(controlClass);
    lilv_node_free(inputClass);
}

void LV2Plugin::initializePorts() {
    if (!plugin_) {
        return;
    }

    controlPorts_.clear();
    controlPortIndices_.clear();
    audioInputBuffers_.clear();
    audioOutputBuffers_.clear();
    audioInputPorts_.clear();
    audioOutputPorts_.clear();
    atomPortBuffers_.clear();
    atomPorts_.clear();

    uint32_t numPorts = lilv_plugin_get_num_ports(plugin_);
    LilvNode* audioClass = lilv_new_uri(world_, LILV_URI_AUDIO_PORT);
    LilvNode* controlClass = lilv_new_uri(world_, LILV_URI_CONTROL_PORT);
    LilvNode* atomClass = lilv_new_uri(world_, LILV_URI_ATOM_PORT);
    LilvNode* inputClass = lilv_new_uri(world_, LILV_URI_INPUT_PORT);

    for (uint32_t i = 0; i < numPorts; ++i) {
        const LilvPort* port = lilv_plugin_get_port_by_index(plugin_, i);
        if (!port) continue;

        bool isAudio = lilv_port_is_a(plugin_, port, audioClass);
        bool isControl = lilv_port_is_a(plugin_, port, controlClass);
        bool isAtom = lilv_port_is_a(plugin_, port, atomClass);
        bool isInput = lilv_port_is_a(plugin_, port, inputClass);

        if (isControl) {
            float defaultVal = 0.0f;
            LilvNode* defNode = nullptr;
            LilvNode* minNode = nullptr;
            LilvNode* maxNode = nullptr;
            lilv_port_get_range(plugin_, port, &defNode, &minNode, &maxNode);
            if (defNode) {
                defaultVal = static_cast<float>(lilv_node_as_float(defNode));
                lilv_node_free(defNode);
            }
            if (minNode) lilv_node_free(minNode);
            if (maxNode) lilv_node_free(maxNode);
            controlPorts_.push_back(std::unique_ptr<float>(new float(defaultVal)));
            controlPortIndices_.push_back(i);
        } else if (isAudio) {
            if (isInput) {
                audioInputBuffers_.emplace_back(kMaxLv2BufferFrames, 0.0f);
                audioInputPorts_.push_back(audioInputBuffers_.back().data());
            } else {
                audioOutputBuffers_.emplace_back(kMaxLv2BufferFrames, 0.0f);
                audioOutputPorts_.push_back(audioOutputBuffers_.back().data());
            }
        } else if (isAtom) {
            size_t bufIdx = atomPortBuffers_.size();
            atomPortBuffers_.emplace_back(kAtomBufferSize, 0);
            atomPorts_.push_back({i, isInput, bufIdx});
        }
    }

    lilv_node_free(audioClass);
    lilv_node_free(controlClass);
    lilv_node_free(atomClass);
    lilv_node_free(inputClass);

    LOGI("initializePorts: control=%zu audioIn=%zu audioOut=%zu atom=%zu",
         controlPorts_.size(), audioInputPorts_.size(), audioOutputPorts_.size(),
         atomPorts_.size());
}

// ---------- State path mapping ----------

char* LV2Plugin::mapAbstractPathCallback(LV2_State_Map_Path_Handle handle, const char* absolutePath) {
    auto* self = static_cast<LV2Plugin*>(handle);
    std::string abs(absolutePath ? absolutePath : "");
    std::string result;

    // Strip filesDir prefix to produce a relative (abstract) path
    if (!self->filesDir_.empty() && abs.size() > self->filesDir_.size() + 1 &&
        abs.compare(0, self->filesDir_.size(), self->filesDir_) == 0 &&
        abs[self->filesDir_.size()] == '/') {
        result = abs.substr(self->filesDir_.size() + 1);
    } else {
        // Not under filesDir — keep absolute
        result = abs;
    }

    char* ret = static_cast<char*>(malloc(result.size() + 1));
    if (ret) memcpy(ret, result.c_str(), result.size() + 1);
    return ret;
}

char* LV2Plugin::mapAbsolutePathCallback(LV2_State_Map_Path_Handle handle, const char* abstractPath) {
    auto* self = static_cast<LV2Plugin*>(handle);
    std::string abst(abstractPath ? abstractPath : "");
    std::string result;

    // If relative, prepend filesDir
    if (!abst.empty() && abst[0] != '/') {
        result = self->filesDir_ + "/" + abst;
    } else {
        result = abst;
    }

    char* ret = static_cast<char*>(malloc(result.size() + 1));
    if (ret) memcpy(ret, result.c_str(), result.size() + 1);
    return ret;
}

void LV2Plugin::freePathCallback(LV2_State_Free_Path_Handle /*handle*/, char* path) {
    free(path);
}

// ---------- State save/restore ----------

PluginState LV2Plugin::saveState() {
    PluginState state;
    if (!plugin_) return state;

    // Plugin URI
    const LilvNode* uri = lilv_plugin_get_uri(plugin_);
    if (uri) state.pluginUri = lilv_node_as_string(uri);

    // Control port values
    for (size_t k = 0; k < controlPorts_.size(); ++k) {
        if (controlPorts_[k] && k < controlPortIndices_.size()) {
            state.controlPortValues.emplace_back(controlPortIndices_[k], *controlPorts_[k]);
        }
    }

    // State properties via state:interface
    if (stateInterface_ && stateInterface_->save && instance_) {
        struct SaveContext {
            std::vector<StateProperty>* props;
            UridMapImpl* uridMap;
        };
        SaveContext ctx{&state.properties, &getGlobalUridMap()};

        auto storeCallback = [](LV2_State_Handle handle, uint32_t key,
                                const void* value, size_t size,
                                uint32_t type, uint32_t flags) -> LV2_State_Status {
            auto* c = static_cast<SaveContext*>(handle);
            StateProperty prop;
            const char* keyStr = c->uridMap->unmap(key);
            const char* typeStr = c->uridMap->unmap(type);
            if (!keyStr) return LV2_STATE_ERR_UNKNOWN;
            prop.keyUri = keyStr;
            prop.typeUri = typeStr ? typeStr : "";
            prop.flags = flags;
            if (value && size > 0) {
                auto* bytes = static_cast<const uint8_t*>(value);
                prop.value.assign(bytes, bytes + size);
            }
            c->props->push_back(std::move(prop));
            return LV2_STATE_SUCCESS;
        };

        const LV2_Feature* stateFeatures[] = {
            &mapPathFeature_, &freePathFeature_, nullptr
        };

        LV2_Handle lv2Handle = lilv_instance_get_handle(instance_);
        LV2_State_Status status = stateInterface_->save(
            lv2Handle, storeCallback, &ctx,
            LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, stateFeatures);

        LOGI("saveState: %zu properties, status=%d", state.properties.size(), status);
    }

    return state;
}

bool LV2Plugin::restoreState(const PluginState& state) {
    if (!instance_) return false;

    // Restore control port values
    for (const auto& [portIndex, value] : state.controlPortValues) {
        setParameter(portIndex, value);
    }

    // Restore state properties via state:interface
    if (stateInterface_ && stateInterface_->restore && !state.properties.empty()) {
        struct RestoreContext {
            const std::vector<StateProperty>* props;
            UridMapImpl* uridMap;
        };
        RestoreContext ctx{&state.properties, &getGlobalUridMap()};

        auto retrieveCallback = [](LV2_State_Handle handle, uint32_t key,
                                   size_t* size, uint32_t* type,
                                   uint32_t* flags) -> const void* {
            auto* c = static_cast<RestoreContext*>(handle);
            const char* keyStr = c->uridMap->unmap(key);
            if (!keyStr) return nullptr;

            for (const auto& prop : *c->props) {
                if (prop.keyUri == keyStr) {
                    if (size) *size = prop.value.size();
                    if (type) *type = c->uridMap->map(prop.typeUri.c_str());
                    if (flags) *flags = prop.flags;
                    return prop.value.empty() ? nullptr : prop.value.data();
                }
            }
            return nullptr;
        };

        const LV2_Feature* stateFeatures[] = {
            &mapPathFeature_, &freePathFeature_, nullptr
        };

        LV2_Handle lv2Handle = lilv_instance_get_handle(instance_);
        LV2_State_Status status = stateInterface_->restore(
            lv2Handle, retrieveCallback, &ctx, 0, stateFeatures);

        LOGI("restoreState: %zu properties, status=%d", state.properties.size(), status);
        return status == LV2_STATE_SUCCESS;
    }

    return true;
}

// ---------- Worker extension ----------

void LV2Plugin::startWorker() {
    if (!instance_) return;

    // Get worker interface from plugin via extension_data
    const void* iface = lilv_instance_get_extension_data(instance_, LV2_WORKER__interface);
    workerInterface_ = static_cast<const LV2_Worker_Interface*>(iface);

    if (!workerInterface_) {
        LOGI("Plugin does not provide worker interface");
        return;
    }

    LOGI("Starting worker thread (work=%p work_response=%p end_run=%p)",
         (void*)workerInterface_->work,
         (void*)workerInterface_->work_response,
         (void*)workerInterface_->end_run);

    workerRunning_.store(true, std::memory_order_release);
    workerThread_ = std::thread(&LV2Plugin::workerThreadFunc, this);
}

void LV2Plugin::stopWorker() {
    if (!workerRunning_.load(std::memory_order_acquire)) return;

    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        workerRunning_.store(false, std::memory_order_release);
    }
    workerCond_.notify_one();

    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    // Clear queues
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        while (!workRequests_.empty()) workRequests_.pop();
    }
    {
        std::lock_guard<std::mutex> lock(responseMutex_);
        while (!workResponses_.empty()) workResponses_.pop();
    }

    workerInterface_ = nullptr;
}

void LV2Plugin::workerThreadFunc() {
    LOGI("Worker thread started");
    while (true) {
        std::vector<uint8_t> data;
        {
            std::unique_lock<std::mutex> lock(workerMutex_);
            workerCond_.wait(lock, [this] {
                return !workRequests_.empty() ||
                       !workerRunning_.load(std::memory_order_acquire);
            });

            if (!workerRunning_.load(std::memory_order_acquire) &&
                workRequests_.empty()) {
                break;
            }

            if (!workRequests_.empty()) {
                data = std::move(workRequests_.front());
                workRequests_.pop();
            }
        }

        if (!data.empty() && workerInterface_ && instance_) {
            LV2_Handle handle = lilv_instance_get_handle(instance_);
            workerInterface_->work(
                handle,
                respondCallback,
                this,
                static_cast<uint32_t>(data.size()),
                data.data());
        }
    }
    LOGI("Worker thread stopped");
}

LV2_Worker_Status LV2Plugin::scheduleWorkCallback(
    LV2_Worker_Schedule_Handle handle, uint32_t size, const void* data)
{
    auto* self = static_cast<LV2Plugin*>(handle);

    std::vector<uint8_t> buf(size);
    if (size > 0 && data) {
        std::memcpy(buf.data(), data, size);
    }

    {
        std::lock_guard<std::mutex> lock(self->workerMutex_);
        self->workRequests_.push(std::move(buf));
    }
    self->workerCond_.notify_one();

    return LV2_WORKER_SUCCESS;
}

LV2_Worker_Status LV2Plugin::respondCallback(
    LV2_Worker_Respond_Handle handle, uint32_t size, const void* data)
{
    auto* self = static_cast<LV2Plugin*>(handle);

    std::vector<uint8_t> buf(size);
    if (size > 0 && data) {
        std::memcpy(buf.data(), data, size);
    }

    {
        std::lock_guard<std::mutex> lock(self->responseMutex_);
        self->workResponses_.push(std::move(buf));
    }

    return LV2_WORKER_SUCCESS;
}

#else // HAVE_LV2 not defined or == 0 - stub implementation

LV2Plugin::LV2Plugin(LilvPlugin_* plugin, LilvWorld_* world, float sampleRate, const std::string& /*filesDir*/)
    : plugin_(plugin)
    , world_(world)
    , instance_(nullptr)
    , sampleRate_(sampleRate)
    , isActive_(false)
{
    LOGE("LV2Plugin: LV2 libraries not available (stub mode)");
}

LV2Plugin::~LV2Plugin() {
    deactivate();
}

void LV2Plugin::activate(float sampleRate, uint32_t /*bufferSize*/) {
    sampleRate_ = sampleRate;
    isActive_ = true;
}

void LV2Plugin::deactivate() {
    isActive_ = false;
}

void LV2Plugin::process(const float* const* inputs, float* const* outputs, uint32_t numFrames) {
    // Passthrough
    if (inputs && outputs && numFrames > 0) {
        for (uint32_t ch = 0; ch < 2; ++ch) {
            if (inputs[ch] && outputs[ch]) {
                std::memcpy(outputs[ch], inputs[ch], numFrames * sizeof(float));
            }
        }
    }
}

PluginInfo LV2Plugin::getInfo() const {
    PluginInfo info;
    info.format = "LV2";
    return info;
}

void LV2Plugin::setParameter(uint32_t portIndex, float value) {
    // Stub
}

float LV2Plugin::getParameter(uint32_t portIndex) const {
    return 0.0f;
}

uint32_t LV2Plugin::getNumInputPorts() const {
    return 0;
}

uint32_t LV2Plugin::getNumOutputPorts() const {
    return 0;
}

void LV2Plugin::setFilePath(const std::string& propertyUri, const std::string& path) {
    // Stub
}

void LV2Plugin::connectPorts() {
    // Stub
}

void LV2Plugin::initializePorts() {
    // Stub
}

#endif // HAVE_LV2 == 1

} // namespace guitarrackcraft
