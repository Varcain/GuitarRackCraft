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

#include "OfflineProcessor.h"
#include "utils/WavIO.h"
#include <algorithm>
#include <cstring>
#include <android/log.h>

#define LOG_TAG "OfflineProcessor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace guitarrackcraft {

OfflineProcessor::OfflineProcessor(PluginChain& chain)
    : chain_(chain)
{
}

bool OfflineProcessor::processFile(
    const std::string& inputPath,
    const std::string& outputPath,
    ProgressCallback progressCallback) {
    
    LOGI("Processing file: %s -> %s", inputPath.c_str(), outputPath.c_str());

    // Read input file
    std::vector<float> inputSamples;
    uint32_t sampleRate = 44100;
    uint32_t numChannels = 1;

    if (!guitarrackcraft::readWavFile(inputPath, inputSamples, sampleRate, numChannels)) {
        LOGE("Failed to read input file");
        return false;
    }

    LOGI("Loaded file: %zu samples, %u Hz, %u channels", 
         inputSamples.size(), sampleRate, numChannels);

    // Activate plugin chain
    chain_.setSampleRate(static_cast<float>(sampleRate));
    chain_.activate();

    // Convert to stereo if needed (guitar effects expect stereo)
    std::vector<float> stereoInput;
    if (numChannels == 1) {
        stereoInput.resize(inputSamples.size() * 2);
        for (size_t i = 0; i < inputSamples.size(); ++i) {
            stereoInput[i * 2] = inputSamples[i];
            stereoInput[i * 2 + 1] = inputSamples[i];
        }
    } else {
        stereoInput = std::move(inputSamples);
    }

    // Process through chain in blocks
    size_t totalFrames = stereoInput.size() / 2;
    std::vector<float> outputSamples(stereoInput.size());
    
    const float* inputPtrs[2];
    float* outputPtrs[2];
    
    std::vector<float> inputLeft(BUFFER_SIZE);
    std::vector<float> inputRight(BUFFER_SIZE);
    std::vector<float> outputLeft(BUFFER_SIZE);
    std::vector<float> outputRight(BUFFER_SIZE);

    for (size_t offset = 0; offset < totalFrames; offset += BUFFER_SIZE) {
        size_t framesToProcess = std::min(BUFFER_SIZE, totalFrames - offset);
        
        // Deinterleave input
        for (size_t i = 0; i < framesToProcess; ++i) {
            size_t idx = (offset + i) * 2;
            inputLeft[i] = stereoInput[idx];
            inputRight[i] = stereoInput[idx + 1];
        }

        // Set up pointers
        inputPtrs[0] = inputLeft.data();
        inputPtrs[1] = inputRight.data();
        outputPtrs[0] = outputLeft.data();
        outputPtrs[1] = outputRight.data();

        // Process
        chain_.process(inputPtrs, outputPtrs, static_cast<uint32_t>(framesToProcess));

        // Interleave output
        for (size_t i = 0; i < framesToProcess; ++i) {
            size_t idx = (offset + i) * 2;
            outputSamples[idx] = outputLeft[i];
            outputSamples[idx + 1] = outputRight[i];
        }

        // Report progress
        if (progressCallback) {
            float progress = static_cast<float>(offset + framesToProcess) / totalFrames;
            progressCallback(progress);
        }
    }

    chain_.deactivate();

    // Write output file
    if (!guitarrackcraft::writeWavFile(outputPath, outputSamples, sampleRate, 2)) {
        LOGE("Failed to write output file");
        return false;
    }

    LOGI("Processing complete");
    return true;
}

} // namespace guitarrackcraft
