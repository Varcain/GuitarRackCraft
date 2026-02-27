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

#include "WavIO.h"
#include <android/log.h>
#include <algorithm>
#include <cstring>
#include <fstream>

#define LOG_TAG "WavIO"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace guitarrackcraft {

bool readWavFile(const std::string& path,
                 std::vector<float>& samples,
                 uint32_t& sampleRate,
                 uint32_t& numChannels) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        LOGE("Cannot open file: %s", path.c_str());
        return false;
    }

    WavHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(WavHeader));

    if (std::memcmp(header.riff, "RIFF", 4) != 0 ||
        std::memcmp(header.wave, "WAVE", 4) != 0 ||
        std::memcmp(header.fmt, "fmt ", 4) != 0 ||
        header.audioFormat != 1) {
        LOGE("Invalid WAV file format");
        return false;
    }

    sampleRate = header.sampleRate;
    numChannels = header.numChannels;
    uint16_t bitsPerSample = header.bitsPerSample;

    if (header.fmtSize > 16) {
        file.seekg(header.fmtSize - 16, std::ios::cur);
    }

    uint32_t dataSize = 0;
    if (std::memcmp(header.data, "data", 4) == 0) {
        dataSize = header.dataSize;
    } else {
        char chunkId[4];
        uint32_t chunkSize = 0;
        while (file.read(chunkId, 4)) {
            file.read(reinterpret_cast<char*>(&chunkSize), 4);
            if (std::memcmp(chunkId, "data", 4) == 0) {
                dataSize = chunkSize;
                break;
            }
            file.seekg(chunkSize, std::ios::cur);
        }
    }

    size_t numSamples = dataSize / (bitsPerSample / 8);
    if (numSamples == 0) {
        LOGE("WAV file has no data");
        return false;
    }
    if (numSamples > kMaxWavSamples) {
        LOGE("WAV file too large: %zu samples", numSamples);
        return false;
    }

    samples.resize(numSamples);

    if (bitsPerSample == 16) {
        std::vector<int16_t> intSamples(numSamples);
        file.read(reinterpret_cast<char*>(intSamples.data()), dataSize);
        for (size_t i = 0; i < numSamples; ++i) {
            samples[i] = intSamples[i] / kInt16MaxF;
        }
    } else if (bitsPerSample == 32) {
        std::vector<int32_t> intSamples(numSamples);
        file.read(reinterpret_cast<char*>(intSamples.data()), dataSize);
        for (size_t i = 0; i < numSamples; ++i) {
            samples[i] = intSamples[i] / kInt32MaxF;
        }
    } else {
        LOGE("Unsupported bit depth: %u", bitsPerSample);
        return false;
    }

    return true;
}

bool writeWavFile(const std::string& path,
                  const std::vector<float>& samples,
                  uint32_t sampleRate,
                  uint32_t numChannels) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        LOGE("Cannot create file: %s", path.c_str());
        return false;
    }

    uint16_t bitsPerSample = 16;
    uint32_t dataSize = static_cast<uint32_t>(samples.size() * (bitsPerSample / 8));
    uint32_t fileSize = 36 + dataSize;

    WavHeader header;
    std::memcpy(header.riff, "RIFF", 4);
    header.fileSize = fileSize - 8;
    std::memcpy(header.wave, "WAVE", 4);
    std::memcpy(header.fmt, "fmt ", 4);
    header.fmtSize = 16;
    header.audioFormat = 1;
    header.numChannels = static_cast<uint16_t>(numChannels);
    header.sampleRate = sampleRate;
    header.byteRate = sampleRate * numChannels * bitsPerSample / 8;
    header.blockAlign = static_cast<uint16_t>(numChannels * bitsPerSample / 8);
    header.bitsPerSample = bitsPerSample;
    std::memcpy(header.data, "data", 4);
    header.dataSize = dataSize;

    file.write(reinterpret_cast<const char*>(&header), sizeof(WavHeader));

    std::vector<int16_t> intSamples(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        float clamped = std::max(-1.0f, std::min(1.0f, samples[i]));
        intSamples[i] = static_cast<int16_t>(clamped * 32767.0f);
    }

    file.write(reinterpret_cast<const char*>(intSamples.data()), dataSize);

    return true;
}

} // namespace guitarrackcraft
