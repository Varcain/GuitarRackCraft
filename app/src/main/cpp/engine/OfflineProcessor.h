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

#ifndef GUITARRACKCRAFT_OFFLINE_PROCESSOR_H
#define GUITARRACKCRAFT_OFFLINE_PROCESSOR_H

#include <string>
#include <functional>
#include "plugin/PluginChain.h"

namespace guitarrackcraft {

/**
 * Processes audio files offline through the plugin chain.
 * Not real-time, so can use blocking operations.
 */
class OfflineProcessor {
public:
    using ProgressCallback = std::function<void(float progress)>;

    OfflineProcessor(PluginChain& chain);
    ~OfflineProcessor() = default;

    /**
     * Process an audio file through the plugin chain.
     * @param inputPath Path to input WAV file
     * @param outputPath Path to output WAV file
     * @param progressCallback Optional callback for progress updates (0.0-1.0)
     * @return true if processing succeeded
     */
    bool processFile(
        const std::string& inputPath,
        const std::string& outputPath,
        ProgressCallback progressCallback = nullptr);

private:
    PluginChain& chain_;
    static constexpr size_t BUFFER_SIZE = 4096;
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_OFFLINE_PROCESSOR_H
