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

#ifndef GUITARRACKCRAFT_AUDIO_RECORDER_H
#define GUITARRACKCRAFT_AUDIO_RECORDER_H

#include "RingBuffer.h"
#include <atomic>
#include <fstream>
#include <string>
#include <thread>

namespace guitarrackcraft {

/**
 * Records raw input (mono) and processed output (stereo) simultaneously.
 *
 * feedAudio() is called from the real-time audio callback — it only writes to
 * lock-free ring buffers. A background writer thread drains the rings and
 * streams PCM data to two WAV files (16-bit).
 */
class AudioRecorder {
public:
    AudioRecorder();
    ~AudioRecorder();

    bool startRecording(const std::string& rawPath, const std::string& processedPath, float sampleRate);
    void stopRecording();
    bool isRecording() const { return recording_.load(std::memory_order_relaxed); }
    double getDurationSec() const;

    /**
     * Feed audio data from the real-time callback.
     * Must be lock-free: only writes to ring buffers.
     */
    void feedAudio(const float* rawMono, const float* processedL, const float* processedR, int32_t numFrames);

private:
    std::atomic<bool> recording_{false};
    std::atomic<bool> writerRunning_{false};
    std::atomic<size_t> totalRawFrames_{0};
    float sampleRate_{48000.0f};

    RingBuffer rawRing_;
    RingBuffer processedRing_;

    std::ofstream rawFile_;
    std::ofstream processedFile_;
    std::thread writerThread_;

    // Temp buffer for interleaving stereo in feedAudio (avoid allocation)
    std::vector<float> interleaveBuffer_;

    void writerLoop();
    void writeWavHeader(std::ofstream& file, uint16_t numChannels, uint32_t sampleRate);
    void finalizeWavFile(std::ofstream& file, size_t totalSamples, uint16_t numChannels);
    void drainRing(RingBuffer& ring, std::ofstream& file, size_t& totalSamples);
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_AUDIO_RECORDER_H
