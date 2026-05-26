#pragma once

#include <cstdint>
#include <string>

extern "C" {
#include "../../../../external/shared_layout.h"
}

// Host-side accessor for the mmap'd VstpocShared region exchanged with the
// x86_64 guest. Creates and zeroes the backing file on construction; unmaps
// and closes (but does not delete) on destruction.
class SharedRing {
public:
    explicit SharedRing(const std::string& path);
    ~SharedRing();

    SharedRing(const SharedRing&) = delete;
    SharedRing& operator=(const SharedRing&) = delete;

    bool valid() const { return data_ != nullptr; }
    VstpocShared* raw() { return data_; }

    // Host (audio thread): pull up to maxFrames stereo samples from the
    // audio ring. Returns frames actually drained (may be < maxFrames on
    // underrun). Non-blocking, lock-free, RT-safe.
    int32_t pullAudio(float* outL, float* outR, int32_t maxFrames);

    // Host (UI thread): enqueue a parameter change for the guest.
    // Drops the message silently if the param ring is full.
    void pushParam(int32_t index, float value);

    // Host (RT input thread): push `numFrames` interleaved-stereo float
    // samples into the mic input ring. Returns frames actually pushed
    // (may be less than numFrames if the ring is near-full).
    // RT-safe, lock-free.
    int32_t pushInput(const float* interleavedStereo, int32_t numFrames);

    // Host: declare that the input ring is being fed by a live mic stream.
    // Guest checks this flag — if set, it reads from audio_in; otherwise
    // it generates a test signal (sawtooth). Cleared on stop.
    void setMicActive(bool active);

    // Host: ask the guest to exit its loop. Sets the shared stop_flag.
    void signalStop();

    bool guestReady() const;
    uint64_t guestFramesProduced() const;

private:
    int fd_ = -1;
    VstpocShared* data_ = nullptr;
};
