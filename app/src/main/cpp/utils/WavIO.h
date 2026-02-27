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

#include <cstdint>
#include <string>
#include <vector>

namespace guitarrackcraft {

struct WavHeader {
    char riff[4];
    uint32_t fileSize;
    char wave[4];
    char fmt[4];
    uint32_t fmtSize;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char data[4];
    uint32_t dataSize;
};

inline constexpr float kInt16MaxF = 32768.0f;
inline constexpr float kInt32MaxF = 2147483648.0f;
inline constexpr size_t kMaxWavSamples = 100000000;

bool readWavFile(const std::string& path,
                 std::vector<float>& samples,
                 uint32_t& sampleRate,
                 uint32_t& numChannels);

bool writeWavFile(const std::string& path,
                  const std::vector<float>& samples,
                  uint32_t sampleRate,
                  uint32_t numChannels);

} // namespace guitarrackcraft
