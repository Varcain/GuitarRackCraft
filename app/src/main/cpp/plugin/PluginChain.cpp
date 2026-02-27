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

#include "PluginChain.h"
#include "../utils/ThreadUtils.h"
#include <android/log.h>
#include <algorithm>
#include <chrono>
#include <cstring>

#define LOG_TAG "PluginChain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace guitarrackcraft {

int PluginChain::addPlugin(std::unique_ptr<IPlugin> plugin, int position) {
    if (!plugin) {
        return -1;
    }

    std::unique_lock lock(chainMutex_);

    int index;
    if (position < 0 || position >= static_cast<int>(plugins_.size())) {
        plugins_.push_back(std::move(plugin));
        index = static_cast<int>(plugins_.size() - 1);
    } else {
        plugins_.insert(plugins_.begin() + position, std::move(plugin));
        index = position;
    }

    // Activate the new plugin with current sample rate so it processes audio.
    // Plugins added after the engine has started would otherwise never be activated.
    if (sampleRate_ > 0.0f) {
        plugins_[index]->activate(sampleRate_, bufferSize_);
    }
    LOGI("addPlugin: index=%d sampleRate=%.0f", index, sampleRate_);
    return index;
}

bool PluginChain::removePlugin(int index) {
    std::unique_lock lock(chainMutex_);
    
    if (index < 0 || index >= static_cast<int>(plugins_.size())) {
        return false;
    }

    plugins_[index]->deactivate();
    plugins_.erase(plugins_.begin() + index);
    return true;
}

bool PluginChain::reorderPlugins(int fromIndex, int toIndex) {
    std::unique_lock lock(chainMutex_);
    
    if (fromIndex < 0 || fromIndex >= static_cast<int>(plugins_.size()) ||
        toIndex < 0 || toIndex >= static_cast<int>(plugins_.size()) ||
        fromIndex == toIndex) {
        return false;
    }

    auto plugin = std::move(plugins_[fromIndex]);
    plugins_.erase(plugins_.begin() + fromIndex);
    
    plugins_.insert(plugins_.begin() + toIndex, std::move(plugin));
    
    return true;
}

void PluginChain::process(const float* const* inputs, float* const* outputs, uint32_t numFrames) {
    // Shared lock so we can run alongside get/setParameter; only fail when add/remove holds exclusive
    std::shared_lock lock(chainMutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        static auto lastLogFail = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastLogFail).count() >= 1.0) {
            lastLogFail = now;
            LOGI("process: lock failed tid=%ld passthrough", getTid());
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

    if (plugins_.empty()) {
        static auto lastLogEmpty = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastLogEmpty).count() >= 1.0) {
            lastLogEmpty = now;
            LOGI("process: chain empty, passthrough");
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

    // Rate-limited: confirm we're running the chain (helps debug shutdown race)
    {
        static auto lastLogRun = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastLogRun).count() >= 1.0) {
            lastLogRun = now;
            LOGI("process: running chain tid=%ld size=%zu", getTid(), plugins_.size());
        }
    }

    // Process through chain
    const float* currentInputs[2] = {inputs[0], inputs[1]};
    float* currentOutputs[2] = {nullptr, nullptr};

    ensureBuffers(numFrames, 2);

    for (size_t i = 0; i < plugins_.size(); ++i) {
        auto& plugin = plugins_[i];
        
        // Set up outputs
        if (i == plugins_.size() - 1) {
            // Last plugin writes to final outputs
            currentOutputs[0] = outputs[0];
            currentOutputs[1] = outputs[1];
        } else {
            // Intermediate plugins write to buffers
            currentOutputs[0] = intermediateBuffers_[0].data();
            currentOutputs[1] = intermediateBuffers_[1].data();
        }

        // Process
        const float* const inputPtrs[2] = {currentInputs[0], currentInputs[1]};
        plugin->process(inputPtrs, currentOutputs, numFrames);

        // Next plugin's input is this plugin's output
        if (i < plugins_.size() - 1) {
            currentInputs[0] = intermediateBuffers_[0].data();
            currentInputs[1] = intermediateBuffers_[1].data();
        }
    }
}

void PluginChain::setSampleRate(float sampleRate, uint32_t bufferSize) {
    std::unique_lock lock(chainMutex_);
    sampleRate_ = sampleRate;
    bufferSize_ = bufferSize;
    for (auto& plugin : plugins_) {
        plugin->activate(sampleRate, bufferSize);
    }
}

void PluginChain::activate() {
    // No-op: plugins are activated individually in setSampleRate() and addPlugin().
}

void PluginChain::deactivate() {
    LOGI("deactivate() entered tid=%ld", getTid());
    std::unique_lock lock(chainMutex_);
    LOGI("deactivate() chainMutex_ acquired tid=%ld", getTid());
    for (auto& plugin : plugins_) {
        plugin->deactivate();
    }
    LOGI("deactivate() done tid=%ld", getTid());
}

size_t PluginChain::getSize() const {
    std::shared_lock lock(chainMutex_);
    return plugins_.size();
}

IPlugin* PluginChain::getPlugin(int index) {
    std::shared_lock lock(chainMutex_);
    if (index < 0 || index >= static_cast<int>(plugins_.size())) {
        return nullptr;
    }
    return plugins_[index].get();
}

void PluginChain::setParameter(int pluginIndex, uint32_t portIndex, float value) {
    std::shared_lock lock(chainMutex_);
    if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
        return;
    }
    plugins_[pluginIndex]->setParameter(portIndex, value);
}

float PluginChain::getParameter(int pluginIndex, uint32_t portIndex) const {
    std::shared_lock lock(chainMutex_);
    if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
        return 0.0f;
    }
    return plugins_[pluginIndex]->getParameter(portIndex);
}

void PluginChain::setPluginFilePath(int pluginIndex, const std::string& propertyUri, const std::string& path) {
    std::shared_lock lock(chainMutex_);
    if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
        return;
    }
    plugins_[pluginIndex]->setFilePath(propertyUri, path);
}

void PluginChain::injectAtom(int pluginIndex, const void* data, uint32_t size) {
    std::shared_lock lock(chainMutex_);
    if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
        return;
    }
    plugins_[pluginIndex]->injectAtom(data, size);
}

PluginChain::ChainState PluginChain::saveChainState() {
    std::shared_lock lock(chainMutex_);
    ChainState cs;
    cs.plugins.reserve(plugins_.size());
    for (auto& plugin : plugins_) {
        cs.plugins.push_back(plugin->saveState());
    }
    LOGI("saveChainState: %zu plugins", cs.plugins.size());
    return cs;
}

bool PluginChain::restorePluginState(int index, const PluginState& state) {
    std::unique_lock lock(chainMutex_);
    if (index < 0 || index >= static_cast<int>(plugins_.size())) {
        return false;
    }
    bool ok = plugins_[index]->restoreState(state);
    LOGI("restorePluginState: index=%d ok=%d", index, ok);
    return ok;
}

void PluginChain::ensureBuffers(uint32_t numFrames, uint32_t numChannels) {
    if (intermediateBuffers_.size() < numChannels) {
        intermediateBuffers_.resize(numChannels);
    }
    for (auto& buffer : intermediateBuffers_) {
        if (buffer.size() < numFrames) {
            buffer.resize(numFrames);
        }
    }
}

} // namespace guitarrackcraft
