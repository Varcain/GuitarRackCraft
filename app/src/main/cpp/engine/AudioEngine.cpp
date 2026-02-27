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

#include "AudioEngine.h"
#include "utils/WavIO.h"
#include "utils/ThreadUtils.h"
#include <oboe/OboeExtensions.h>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <thread>

#define LOG_TAG "AudioEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace guitarrackcraft {

AudioEngine::AudioEngine()
    : sampleRate_(48000.0f)
    , isRunning_(false)
{
    inputPtrs_[0] = nullptr;
    inputPtrs_[1] = nullptr;
    outputPtrs_[0] = nullptr;
    outputPtrs_[1] = nullptr;
}

AudioEngine::~AudioEngine() {
    stop();
}

bool AudioEngine::start(float sampleRate, int32_t inputDeviceId,
                        int32_t outputDeviceId, int32_t bufferFrames) {
    LOGI("start() ENTER tid=%ld sampleRate=%.0f inputDev=%d outputDev=%d bufFrames=%d isRunning_=%d",
         getTid(), sampleRate, inputDeviceId, outputDeviceId, bufferFrames, isRunning_ ? 1 : 0);
    if (isRunning_) {
        LOGI("start() early return (already running)");
        return true;
    }

    sampleRate_ = sampleRate;
    inputDeviceId_ = inputDeviceId;
    outputDeviceId_ = outputDeviceId;
    requestedBufferFrames_ = bufferFrames;

    if (!createAudioStreams(sampleRate)) {
        LOGE("Failed to create audio streams");
        return false;
    }

    // Activate plugin chain with the power-of-2 callback frame count
    // so convolver plugins configure their partition size correctly
    LOGI("Using callback frame count: %u (power-of-2)", callbackFrameCount_);
    chain_.setSampleRate(sampleRate_, callbackFrameCount_);
    chain_.activate();

    isRunning_ = true;
    LOGI("start() EXIT tid=%ld Audio engine started at %.0f Hz", getTid(), sampleRate_);
    return true;
}

void AudioEngine::stop() {
    LOGI("stop() entered tid=%ld isRunning_=%d", getTid(), isRunning_ ? 1 : 0);
    if (!isRunning_) {
        // Stream may have been closed by onErrorAfterClose (e.g. system closed stream when opening X11 UI).
        // We must still call closeStreams() so streams are torn down with the 250ms wait before the
        // destructor runs. Otherwise ~AudioEngine destroys outputStream_/inputStream_ while the
        // AudioTrack callback thread is still in getStream() -> pthread_mutex_lock on destroyed mutex (SIGABRT).
        LOGI("stop() isRunning_=0; calling closeStreams() anyway so streams tear down safely");
        closeStreams();
        return;
    }
    // Stop recording before tearing down the audio path
    if (recorder_.isRecording()) {
        recorder_.stopRecording();
    }

    // Signal callback to exit immediately so it does not touch chain_ or stream
    // during teardown (avoids use-after-free / destroyed mutex in plugin chain or Oboe).
    isRunning_ = false;
    LOGI("stop() isRunning_=false set, calling chain_.deactivate()");

    chain_.deactivate();
    LOGI("stop() chain_.deactivate() done, calling closeStreams()");

    closeStreams();
    LOGI("stop() done");
}

bool AudioEngine::isRunning() const {
    return isRunning_;
}

AudioEngine::StreamInfo AudioEngine::getStreamInfo() const {
    StreamInfo info;
    if (outputStream_) {
        info.isAAudio = outputStream_->getAudioApi() == oboe::AudioApi::AAudio;
        info.outputExclusive = outputStream_->getSharingMode() == oboe::SharingMode::Exclusive;
        info.outputLowLatency = outputStream_->getPerformanceMode() == oboe::PerformanceMode::LowLatency;
        info.outputMMap = oboe::OboeExtensions::isMMapUsed(outputStream_.get());
        info.outputCallback = true; // always using callback
        info.framesPerBurst = outputStream_->getFramesPerBurst();
    }
    if (inputStream_) {
        info.inputExclusive = inputStream_->getSharingMode() == oboe::SharingMode::Exclusive;
        info.inputLowLatency = inputStream_->getPerformanceMode() == oboe::PerformanceMode::LowLatency;
    }
    return info;
}

double AudioEngine::getLatencyMs() const {
    if (!outputStream_) {
        return 0.0;
    }
    
    int64_t framesWritten = outputStream_->getFramesWritten();
    int64_t framesRead = outputStream_->getFramesRead();
    int32_t bufferSize = outputStream_->getBufferSizeInFrames();
    
    double latencyFrames = bufferSize + (framesWritten - framesRead);
    return (latencyFrames / sampleRate_) * 1000.0;
}

float AudioEngine::getInputLevel() const {
    return inputPeakLevel_.load();
}

float AudioEngine::getOutputLevel() const {
    return outputPeakLevel_.load();
}

float AudioEngine::getCpuLoad() const {
    return cpuLoad_.load();
}

int32_t AudioEngine::getXRunCount() const {
    if (!outputStream_) return 0;
    auto result = outputStream_->getXRunCount();
    return result ? result.value() : 0;
}

bool AudioEngine::isInputClipping() const {
    return inputClipping_.load();
}

bool AudioEngine::isOutputClipping() const {
    return outputClipping_.load();
}

void AudioEngine::resetClipping() {
    inputClipping_.store(false);
    outputClipping_.store(false);
}

// --- WAV playback ---

bool AudioEngine::loadWav(const std::string& path) {
    if (!isRunning_) {
        LOGE("loadWav: engine not running");
        return false;
    }
    wavPause();
    std::vector<float> samples;
    uint32_t fileRate = 44100;
    uint32_t numChannels = 1;
    if (!guitarrackcraft::readWavFile(path, samples, fileRate, numChannels)) {
        return false;
    }
    // Convert to mono if stereo (samples are interleaved L,R,L,R,...)
    std::vector<float> mono;
    if (numChannels == 2) {
        size_t numFrames = samples.size() / 2;
        mono.resize(numFrames);
        for (size_t i = 0; i < numFrames; ++i) {
            mono[i] = (samples[i * 2] + samples[i * 2 + 1]) * 0.5f;
        }
    } else {
        mono = std::move(samples);
    }
    wavBuffer_.clear();
    resampleToEngineRate(mono, fileRate, wavBuffer_);
    wavLengthFrames_ = wavBuffer_.size();
    wavPositionFrames_.store(0);
    wavPlaying_.store(false);
    LOGI("WAV loaded: %zu frames at %.0f Hz (from %u Hz)", wavLengthFrames_, sampleRate_, fileRate);
    return true;
}

void AudioEngine::unloadWav() {
    wavPlaying_.store(false);
    wavBuffer_.clear();
    wavLengthFrames_ = 0;
    wavPositionFrames_.store(0);
}

void AudioEngine::wavPlay() {
    if (!wavBuffer_.empty()) {
        wavPlaying_.store(true);
    }
}

void AudioEngine::wavPause() {
    wavPlaying_.store(false);
}

void AudioEngine::wavSeekToFrame(size_t frame) {
    size_t end = wavLengthFrames_;
    if (end > 0 && frame > end) {
        frame = end;
    }
    wavPositionFrames_.store(frame);
}

double AudioEngine::getWavDurationSec() const {
    if (sampleRate_ <= 0.0f || wavLengthFrames_ == 0) return 0.0;
    return static_cast<double>(wavLengthFrames_) / sampleRate_;
}

double AudioEngine::getWavPositionSec() const {
    if (sampleRate_ <= 0.0f) return 0.0;
    return static_cast<double>(wavPositionFrames_.load()) / sampleRate_;
}

bool AudioEngine::isWavPlaying() const {
    return wavPlaying_.load();
}

bool AudioEngine::isWavLoaded() const {
    return !wavBuffer_.empty();
}


void AudioEngine::resampleToEngineRate(const std::vector<float>& src,
                                       uint32_t srcRate,
                                       std::vector<float>& dst) {
    if (src.empty() || sampleRate_ <= 0.0f) {
        dst.clear();
        return;
    }
    if (static_cast<float>(srcRate) == sampleRate_) {
        dst = src;
        return;
    }
    double ratio = sampleRate_ / static_cast<double>(srcRate);
    size_t outFrames = static_cast<size_t>(std::round(src.size() * ratio));
    if (outFrames == 0) {
        dst.clear();
        return;
    }
    dst.resize(outFrames);
    for (size_t i = 0; i < outFrames; ++i) {
        double srcIdx = i / ratio;
        size_t j = static_cast<size_t>(srcIdx);
        float frac = static_cast<float>(srcIdx - j);
        if (j + 1 >= src.size()) {
            dst[i] = src[src.size() - 1];
        } else {
            dst[i] = src[j] * (1.0f - frac) + src[j + 1] * frac;
        }
    }
}

oboe::DataCallbackResult AudioEngine::onAudioReady(
    oboe::AudioStream* audioStream,
    void* audioData,
    int32_t numFrames) {
    // Debug: log callback thread still active (rate-limited) to correlate with closeStreams() tid
    {
        static std::atomic<int> enterCount{0};
        static auto lastEnterLog = std::chrono::steady_clock::now();
        int c = enterCount++;
        auto now = std::chrono::steady_clock::now();
        if (c < 3 || std::chrono::duration<double>(now - lastEnterLog).count() >= 5.0) {
            if (c >= 3) lastEnterLog = now;
            LOGI("onAudioReady ENTER tid=%ld (callback thread, count=%d)", getTid(), c);
        }
    }
    // Debug: log when callback bails due to shutdown (rate-limited)
    if (!isRunning_ || numFrames <= 0) {
        static std::atomic<int> bailCount{0};
        int c = bailCount++;
        if (c < 5 || (c % 50 == 0)) {
            LOGI("onAudioReady: bail tid=%ld !isRunning_=%d numFrames=%d (bail #%d)",
                 getTid(), isRunning_ ? 0 : 1, numFrames, c);
        }
        return oboe::DataCallbackResult::Continue;
    }
    // Only process when the output stream needs data (we do not set callback on input).
    if (audioStream != outputStream_.get()) {
        return oboe::DataCallbackResult::Continue;
    }

    // Input source: WAV playback or microphone
    const bool useWav = wavPlaying_.load() && !wavBuffer_.empty();
    if (useWav) {
        size_t pos = wavPositionFrames_.load();
        size_t len = wavLengthFrames_;
        size_t toCopy = std::min(static_cast<size_t>(numFrames), len > pos ? len - pos : 0);
        if (toCopy > 0) {
            std::memcpy(inputBuffer_.data(), wavBuffer_.data() + pos, toCopy * sizeof(float));
            wavPositionFrames_.store(pos + toCopy);
        }
        if (toCopy < static_cast<size_t>(numFrames)) {
            std::memset(inputBuffer_.data() + toCopy, 0, (numFrames - toCopy) * sizeof(float));
            wavPlaying_.store(false);
        }
    } else {
        int32_t framesRead = 0;
        if (inputStream_) {
            auto result = inputStream_->read(inputBuffer_.data(), numFrames, 0);
            if (result == oboe::Result::OK) {
                framesRead = result.value();
            }
        }
        if (framesRead < numFrames) {
            std::memset(inputBuffer_.data() + framesRead, 0, (numFrames - framesRead) * sizeof(float));
        }
    }

    // Input peak metering and clipping
    float inputPeak = 0.0f;
    bool inputClip = false;
    for (int32_t i = 0; i < numFrames; ++i) {
        float s = std::fabs(inputBuffer_[i]);
        if (s > inputPeak) inputPeak = s;
        if (s >= kClippingThreshold) inputClip = true;
    }
    inputPeakHold_ = std::max(inputPeak, inputPeakHold_ * kPeakDecay);
    inputPeakLevel_.store(inputPeakHold_);
    if (inputClip) inputClipping_.store(true);

    // Rate-limited debug: input source and level (once per second)
    {
        static auto lastLog = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastLog).count() >= 1.0) {
            lastLog = now;
            LOGI("onAudioReady: source=%s numFrames=%d inputPeak=%.4f",
                 useWav ? "WAV" : "mic", numFrames, inputPeak);
        }
    }

    // Ensure buffers are large enough
    if (inputBuffer_.size() < static_cast<size_t>(numFrames)) {
        inputBuffer_.resize(numFrames);
    }
    if (outputBufferLeft_.size() < static_cast<size_t>(numFrames)) {
        outputBufferLeft_.resize(numFrames);
        outputBufferRight_.resize(numFrames);
    }

    // Set up input pointers (mono guitar input -> stereo)
    inputPtrs_[0] = inputBuffer_.data();
    inputPtrs_[1] = inputBuffer_.data();  // Duplicate mono to stereo

    // Set up output pointers (always process into our buffers for metering)
    float* outputData = static_cast<float*>(audioData);
    int32_t numChannels = audioStream->getChannelCount();
    outputPtrs_[0] = outputBufferLeft_.data();
    outputPtrs_[1] = outputBufferRight_.data();

    // Skip chain processing when bypassed (during preset load) or WAV bypass active
    if (chainBypass_.load() || (useWav && wavBypassChain_.load())) {
        for (int32_t ch = 0; ch < 2; ++ch) {
            std::memcpy(outputPtrs_[ch], inputPtrs_[ch],
                        numFrames * sizeof(float));
        }
    } else {
        // Process through plugin chain (measure CPU time)
        double bufferDurationMs = (numFrames / static_cast<double>(sampleRate_)) * 1000.0;
        auto t0 = std::chrono::high_resolution_clock::now();
        chain_.process(inputPtrs_, outputPtrs_, numFrames);
        auto t1 = std::chrono::high_resolution_clock::now();
        double processMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        float load = static_cast<float>(processMs / bufferDurationMs);
        if (load > 1.0f) load = 1.0f;
        cpuLoad_.store(load);
    }

    // Output peak metering (from buffers we wrote to)
    float outputPeak = 0.0f;
    bool outputClip = false;
    for (int32_t i = 0; i < numFrames; ++i) {
        float s = std::max(std::fabs(outputBufferLeft_[i]), std::fabs(outputBufferRight_[i]));
        if (s > outputPeak) outputPeak = s;
        if (s >= kClippingThreshold) outputClip = true;
    }
    outputPeakHold_ = std::max(outputPeak, outputPeakHold_ * kPeakDecay);
    outputPeakLevel_.store(outputPeakHold_);
    if (outputClip) outputClipping_.store(true);

    // Feed recorder (lock-free ring buffer write)
    if (recorder_.isRecording()) {
        recorder_.feedAudio(inputBuffer_.data(),
                            outputBufferLeft_.data(),
                            outputBufferRight_.data(),
                            numFrames);
    }

    // Rate-limited debug: output level (once per second)
    {
        static auto lastLogOut = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastLogOut).count() >= 1.0) {
            lastLogOut = now;
            LOGI("onAudioReady: outputPeak=%.4f", outputPeak);
        }
    }

    // Copy to output (stereo: deinterleave; mono: mix)
    if (numChannels == 2) {
        for (int32_t i = 0; i < numFrames; ++i) {
            outputData[i * 2] = outputBufferLeft_[i];
            outputData[i * 2 + 1] = outputBufferRight_[i];
        }
    } else {
        for (int32_t i = 0; i < numFrames; ++i) {
            outputData[i] = (outputBufferLeft_[i] + outputBufferRight_[i]) * 0.5f;
        }
    }

    return oboe::DataCallbackResult::Continue;
}

void AudioEngine::onErrorBeforeClose(oboe::AudioStream* oboeStream, oboe::Result error) {
    LOGE("onErrorBeforeClose tid=%ld stream=%p error=%s (set isRunning_=false so callback bails)",
         getTid(), static_cast<void*>(oboeStream), oboe::convertToText(error));
    // Signal callback to exit immediately; Oboe will close the stream after we return.
    isRunning_ = false;
}

void AudioEngine::onErrorAfterClose(oboe::AudioStream* oboeStream, oboe::Result error) {
    LOGE("onErrorAfterClose tid=%ld stream=%p error=%s", getTid(), static_cast<void*>(oboeStream), oboe::convertToText(error));
    isRunning_ = false;
    // Do NOT reset() the stream here. The underlying AAudio stream is already closed by
    // Oboe/the system (e.g. AudioBoost cancelling boost can trigger this). If we run
    // outputStream_.reset() on this thread, the destructor (~AAudioLoader) runs while
    // the audio callback thread may still be inside Oboe -> pthread_mutex_lock on
    // destroyed mutex (SIGABRT). Leave the stream object alive; stop() -> closeStreams()
    // will run later (from lifecycle or user) and destroy it on the main thread after
    // the 250ms sleep, when the callback thread is guaranteed idle.
}

bool AudioEngine::createAudioStreams(float sampleRate) {
    // Enable MMAP data path for lowest latency (must be set before opening streams).
    // Without this, AAudio uses the legacy non-MMAP path and cannot grant exclusive mode.
    oboe::OboeExtensions::setMMapEnabled(true);
    LOGI("MMAP supported=%d enabled=%d", oboe::OboeExtensions::isMMapSupported(),
         oboe::OboeExtensions::isMMapEnabled());

    // --- Input stream (mono, for guitar) ---
    // Force AAudio API — OpenSL ES cannot do exclusive or MMAP.
    // Oboe's QuirksManager may silently choose OpenSL ES otherwise.
    // Use a dedicated builder to avoid leaking input-only settings to output.
    // Do NOT set a callback on input — only the output stream drives the callback.
    // We read from the input inside the output stream's onAudioReady.
    oboe::AudioStreamBuilder inputBuilder;
    inputBuilder.setDirection(oboe::Direction::Input)
           ->setAudioApi(oboe::AudioApi::AAudio)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(1)
           ->setSampleRate(static_cast<int32_t>(sampleRate))
           ->setInputPreset(oboe::InputPreset::VoiceRecognition);

    if (inputDeviceId_ != 0) {
        inputBuilder.setDeviceId(inputDeviceId_);
        LOGI("Input device ID set to %d", inputDeviceId_);
    }

    oboe::AudioStream* inputStreamPtr = nullptr;
    oboe::Result result = inputBuilder.openStream(&inputStreamPtr);
    if (result != oboe::Result::OK) {
        // AAudio failed — retry without forcing API (let Oboe pick)
        LOGE("AAudio input open failed (%s), retrying with default API", oboe::convertToText(result));
        inputBuilder.setAudioApi(oboe::AudioApi::Unspecified);
        result = inputBuilder.openStream(&inputStreamPtr);
        if (result != oboe::Result::OK) {
            LOGE("Failed to open input stream: %s", oboe::convertToText(result));
            return false;
        }
    }
    inputStream_.reset(inputStreamPtr);

    LOGI("Input stream opened: api=%d sharing=%d perf=%d mmap=%d",
         static_cast<int>(inputStream_->getAudioApi()),
         static_cast<int>(inputStream_->getSharingMode()),
         static_cast<int>(inputStream_->getPerformanceMode()),
         oboe::OboeExtensions::isMMapUsed(inputStream_.get()));

    // Use actual sample rate from stream
    sampleRate_ = static_cast<float>(inputStream_->getSampleRate());

    // --- Output stream (stereo) ---
    oboe::AudioStreamBuilder outputBuilder;
    outputBuilder.setDirection(oboe::Direction::Output)
           ->setAudioApi(oboe::AudioApi::AAudio)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(2)
           ->setSampleRate(static_cast<int32_t>(sampleRate_))
           ->setUsage(oboe::Usage::Game)
           ->setCallback(this);

    if (outputDeviceId_ != 0) {
        outputBuilder.setDeviceId(outputDeviceId_);
        LOGI("Output device ID set to %d", outputDeviceId_);
    }

    oboe::AudioStream* outputStreamPtr = nullptr;
    result = outputBuilder.openStream(&outputStreamPtr);
    if (result != oboe::Result::OK) {
        // AAudio failed — retry without forcing API
        LOGE("AAudio output open failed (%s), retrying with default API", oboe::convertToText(result));
        outputBuilder.setAudioApi(oboe::AudioApi::Unspecified);
        result = outputBuilder.openStream(&outputStreamPtr);
        if (result != oboe::Result::OK) {
            LOGE("Failed to open output stream: %s", oboe::convertToText(result));
            closeStreams();
            return false;
        }
    }

    LOGI("Output stream opened: api=%d sharing=%d perf=%d mmap=%d",
         static_cast<int>(outputStreamPtr->getAudioApi()),
         static_cast<int>(outputStreamPtr->getSharingMode()),
         static_cast<int>(outputStreamPtr->getPerformanceMode()),
         oboe::OboeExtensions::isMMapUsed(outputStreamPtr));

    // Determine callback block size.
    // If the user requested a specific buffer size, use it directly.
    // Otherwise, ensure power-of-2 for convolver plugin compatibility.
    {
        if (requestedBufferFrames_ > 0) {
            callbackFrameCount_ = static_cast<uint32_t>(requestedBufferFrames_);
            LOGI("Using user-requested buffer frames: %u", callbackFrameCount_);
            outputStreamPtr->close();
            delete outputStreamPtr;
            outputStreamPtr = nullptr;

            outputBuilder.setFramesPerCallback(requestedBufferFrames_);
            result = outputBuilder.openStream(&outputStreamPtr);
            if (result != oboe::Result::OK) {
                LOGE("Failed to reopen output stream with requested buffer: %s",
                     oboe::convertToText(result));
                closeStreams();
                return false;
            }
        } else {
            int32_t framesPerBurst = outputStreamPtr->getFramesPerBurst();
            uint32_t po2 = 1;
            while (po2 < static_cast<uint32_t>(framesPerBurst)) po2 <<= 1;
            callbackFrameCount_ = po2;

            bool isPo2 = (framesPerBurst > 0) &&
                          ((framesPerBurst & (framesPerBurst - 1)) == 0);
            if (!isPo2) {
                LOGI("framesPerBurst=%d not power-of-2, reopening with framesPerCallback=%u",
                     framesPerBurst, po2);
                outputStreamPtr->close();
                delete outputStreamPtr;
                outputStreamPtr = nullptr;

                outputBuilder.setFramesPerCallback(static_cast<int32_t>(po2));
                result = outputBuilder.openStream(&outputStreamPtr);
                if (result != oboe::Result::OK) {
                    LOGE("Failed to reopen output stream with po2 callback: %s",
                         oboe::convertToText(result));
                    closeStreams();
                    return false;
                }
            } else {
                LOGI("framesPerBurst=%d already power-of-2", framesPerBurst);
            }
        }
    }

    outputStream_.reset(outputStreamPtr);

    // Start streams
    result = inputStream_->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start input stream: %s", oboe::convertToText(result));
        closeStreams();
        return false;
    }

    result = outputStream_->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start output stream: %s", oboe::convertToText(result));
        closeStreams();
        return false;
    }

    // Allocate buffers
    int32_t bufferSize = outputStream_->getBufferSizeInFrames();
    inputBuffer_.resize(bufferSize);
    outputBufferLeft_.resize(bufferSize);
    outputBufferRight_.resize(bufferSize);

    LOGI("Audio streams created: %d Hz, buffer size: %d frames", 
         static_cast<int>(sampleRate_), bufferSize);

    return true;
}

void AudioEngine::closeStreams() {
    LOGI("closeStreams() ENTER tid=%ld (caller thread; AudioTrack callback is different tid)", getTid());
    
    // First, signal the callback to stop immediately to prevent new callbacks
    // from starting while we're tearing down.
    isRunning_.store(false);
    
    if (inputStream_) {
        LOGI("closeStreams() input stream stop+close+reset");
        inputStream_->stop();
        inputStream_->close();
        inputStream_.reset();
    }

    if (outputStream_) {
        LOGI("closeStreams() output stream stop");
        outputStream_->stop();
        LOGI("closeStreams() output stream close");
        outputStream_->close();
        // Wait for any in-flight AAudio callback to finish before destroying the
        // stream object. close() already removed us from AAudioStreamCollection,
        // so late callbacks will see !isStreamAlive and return Stop. If we reset()
        // too soon, ~AudioStreamAAudio / ~AAudioLoader run while the AudioTrack
        // thread is still in getStream() -> destroyed mutex (SIGABRT).
        // 
        // INCREASED from 250ms to 500ms: The 250ms delay was not sufficient on some
        // devices (e.g., OnePlus) where the audio callback thread takes longer to
        // fully exit, especially when the app is being force-closed or when there
        // are concurrent EGL/GL operations that may interfere with the audio subsystem.
        static constexpr int kStreamCloseSleepMs = 500;
        LOGI("closeStreams() sleep %dms before outputStream_.reset() [tid=%ld]", kStreamCloseSleepMs, getTid());
        std::this_thread::sleep_for(std::chrono::milliseconds(kStreamCloseSleepMs));
        LOGI("closeStreams() outputStream_.reset() NOW tid=%ld", getTid());
        outputStream_.reset();
        LOGI("closeStreams() output stream destroyed tid=%ld", getTid());
    }
    LOGI("closeStreams() done");
}

} // namespace guitarrackcraft
