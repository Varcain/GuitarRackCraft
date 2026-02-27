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

#ifndef GUITARRACKCRAFT_AUDIO_ENGINE_H
#define GUITARRACKCRAFT_AUDIO_ENGINE_H

#include <oboe/Oboe.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "plugin/PluginChain.h"
#include "AudioRecorder.h"

namespace guitarrackcraft {

/**
 * Audio engine using Oboe for low-latency audio I/O.
 * Processes audio through the plugin chain in real-time.
 */
class AudioEngine : public oboe::AudioStreamCallback {
public:
    AudioEngine();
    ~AudioEngine();

    /**
     * Start audio processing.
     * @param sampleRate Desired sample rate (will use device default if not supported)
     * @return true if started successfully
     */
    bool start(float sampleRate = 48000.0f, int32_t inputDeviceId = 0,
               int32_t outputDeviceId = 0, int32_t bufferFrames = 0);

    /**
     * Stop audio processing.
     */
    void stop();

    /**
     * Check if engine is running.
     */
    bool isRunning() const;

    /**
     * Get the plugin chain (for adding/removing plugins).
     */
    PluginChain& getChain() { return chain_; }
    const PluginChain& getChain() const { return chain_; }

    /**
     * Get current sample rate.
     */
    float getSampleRate() const { return sampleRate_; }

    /**
     * Get actual callback frame count (buffer size used by the audio callback).
     */
    uint32_t getCallbackFrameCount() const { return callbackFrameCount_; }

    struct StreamInfo {
        bool isAAudio = false;         // AAudio vs OpenSL ES
        bool inputExclusive = false;   // Exclusive sharing mode granted
        bool outputExclusive = false;
        bool inputLowLatency = false;  // LowLatency performance mode granted
        bool outputLowLatency = false;
        bool outputMMap = false;       // MMAP used (lowest path)
        bool outputCallback = true;    // Using data callback
        int32_t framesPerBurst = 0;    // Hardware burst size
    };

    /**
     * Get stream configuration info for the low-latency checklist.
     */
    StreamInfo getStreamInfo() const;

    /**
     * Get current latency in milliseconds.
     */
    double getLatencyMs() const;

    /**
     * Get input peak level (0.0–1.0).
     */
    float getInputLevel() const;

    /**
     * Get output peak level (0.0–1.0).
     */
    float getOutputLevel() const;

    /**
     * Get CPU load (0.0–1.0) from processing time vs buffer duration.
     */
    float getCpuLoad() const;

    /**
     * Get cumulative audio xrun (underrun/overrun) count from the output stream.
     */
    int32_t getXRunCount() const;

    /**
     * True if input has clipped (peak >= 0.99).
     */
    bool isInputClipping() const;

    /**
     * True if output has clipped (peak >= 0.99).
     */
    bool isOutputClipping() const;

    /**
     * Clear clipping indicators (call when user taps to reset).
     */
    void resetClipping();

    /**
     * Bypass chain processing (audio passthrough). Use during preset loading
     * to prevent the audio thread from processing a partially-built chain.
     */
    void setChainBypass(bool bypass) { chainBypass_.store(bypass); }
    void setWavBypassChain(bool bypass) { wavBypassChain_.store(bypass); }

    /**
     * Get the audio recorder for real-time recording of raw input and processed output.
     */
    AudioRecorder& getRecorder() { return recorder_; }

    // --- WAV real-time playback ---

    /**
     * Load a WAV file for playback. Engine must be running (sample rate known).
     * Converts to mono and resamples to engine rate.
     * @return true on success
     */
    bool loadWav(const std::string& path);

    /**
     * Unload the current WAV and stop playback.
     */
    void unloadWav();

    void wavPlay();
    void wavPause();
    void wavSeekToFrame(size_t frame);

    double getWavDurationSec() const;
    double getWavPositionSec() const;
    bool isWavPlaying() const;
    bool isWavLoaded() const;

    // Oboe AudioStreamCallback implementation
    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream* audioStream,
        void* audioData,
        int32_t numFrames) override;

    void onErrorBeforeClose(oboe::AudioStream* oboeStream, oboe::Result error) override;
    void onErrorAfterClose(oboe::AudioStream* oboeStream, oboe::Result error) override;

private:
    std::unique_ptr<oboe::AudioStream> inputStream_;
    std::unique_ptr<oboe::AudioStream> outputStream_;
    PluginChain chain_;
    float sampleRate_;
    int32_t inputDeviceId_ = 0;
    int32_t outputDeviceId_ = 0;
    int32_t requestedBufferFrames_ = 0;
    uint32_t callbackFrameCount_ = 0;  // Power-of-2 frames per audio callback
    std::atomic<bool> isRunning_;
    std::atomic<bool> chainBypass_{false};  // skip chain processing (passthrough)

    // Audio buffers for processing
    std::vector<float> inputBuffer_;
    std::vector<float> outputBufferLeft_;
    std::vector<float> outputBufferRight_;
    
    // Temporary buffers for plugin chain
    const float* inputPtrs_[2];
    float* outputPtrs_[2];

    // Level metering and CPU (written from audio thread, read from UI)
    std::atomic<float> inputPeakLevel_{0.0f};
    std::atomic<float> outputPeakLevel_{0.0f};
    std::atomic<float> cpuLoad_{0.0f};
    std::atomic<bool> inputClipping_{false};
    std::atomic<bool> outputClipping_{false};
    float inputPeakHold_{0.0f};
    float outputPeakHold_{0.0f};

    static constexpr float kClippingThreshold = 0.99f;
    static constexpr float kPeakDecay = 0.95f;

    // WAV playback state (read in callback; written from load/seek/play/pause)
    std::vector<float> wavBuffer_;
    std::atomic<size_t> wavPositionFrames_{0};
    std::atomic<bool> wavPlaying_{false};
    std::atomic<bool> wavBypassChain_{true};  // true = WAV plays raw (backing track), false = through effects
    size_t wavLengthFrames_{0};

    AudioRecorder recorder_;

    bool createAudioStreams(float sampleRate);
    void closeStreams();
    void resampleToEngineRate(const std::vector<float>& src, uint32_t srcRate,
                              std::vector<float>& dst);
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_AUDIO_ENGINE_H
