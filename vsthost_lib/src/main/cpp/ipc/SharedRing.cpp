#include "SharedRing.h"
#include "../util/log.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

SharedRing::SharedRing(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd_ < 0) {
        LOGE("SharedRing: open(%s) failed: %s", path.c_str(), std::strerror(errno));
        return;
    }
    if (::ftruncate(fd_, sizeof(VstpocShared)) != 0) {
        LOGE("SharedRing: ftruncate failed: %s", std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return;
    }
    void* p = ::mmap(nullptr, sizeof(VstpocShared),
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED) {
        LOGE("SharedRing: mmap failed: %s", std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return;
    }
    data_ = static_cast<VstpocShared*>(p);
    std::memset(data_, 0, sizeof(VstpocShared));
    LOGI("SharedRing: mapped %s (%zu bytes)", path.c_str(), sizeof(VstpocShared));
}

SharedRing::~SharedRing() {
    if (data_) ::munmap(data_, sizeof(VstpocShared));
    if (fd_ >= 0) ::close(fd_);
}

int32_t SharedRing::pullAudio(float* outL, float* outR, int32_t maxFrames) {
    if (!data_) return 0;

    const uint64_t head = __atomic_load_n(&data_->audio_head, __ATOMIC_ACQUIRE);
    const uint64_t tail = __atomic_load_n(&data_->audio_tail, __ATOMIC_RELAXED);
    const uint64_t available = head - tail;
    const uint64_t want = static_cast<uint64_t>(maxFrames);
    const uint64_t take = available < want ? available : want;

    for (uint64_t i = 0; i < take; ++i) {
        const uint64_t slot = (tail + i) & (VSTPOC_AUDIO_RING_FRAMES - 1);
        outL[i] = data_->audio[slot * VSTPOC_CHANNELS + 0];
        outR[i] = data_->audio[slot * VSTPOC_CHANNELS + 1];
    }
    __atomic_store_n(&data_->audio_tail, tail + take, __ATOMIC_RELEASE);
    return static_cast<int32_t>(take);
}

int32_t SharedRing::pushInput(const float* interleavedStereo, int32_t numFrames) {
    if (!data_) return 0;

    const uint64_t head = __atomic_load_n(&data_->audio_in_head, __ATOMIC_RELAXED);
    const uint64_t tail = __atomic_load_n(&data_->audio_in_tail, __ATOMIC_ACQUIRE);
    const uint64_t used = head - tail;
    const uint64_t free_frames = (uint64_t)VSTPOC_AUDIO_RING_FRAMES - used;
    const uint64_t want = (uint64_t)numFrames;
    const uint64_t take = free_frames < want ? free_frames : want;

    for (uint64_t i = 0; i < take; ++i) {
        const uint64_t slot = (head + i) & (VSTPOC_AUDIO_RING_FRAMES - 1);
        data_->audio_in[slot * VSTPOC_CHANNELS + 0] = interleavedStereo[i * 2 + 0];
        data_->audio_in[slot * VSTPOC_CHANNELS + 1] = interleavedStereo[i * 2 + 1];
    }
    __atomic_store_n(&data_->audio_in_head, head + take, __ATOMIC_RELEASE);
    return static_cast<int32_t>(take);
}

void SharedRing::setMicActive(bool active) {
    if (!data_) return;
    __atomic_store_n(&data_->mic_active, active ? 1 : 0, __ATOMIC_RELEASE);
}

void SharedRing::pushParam(int32_t index, float value) {
    if (!data_) return;

    const uint64_t head = __atomic_load_n(&data_->param_head, __ATOMIC_RELAXED);
    const uint64_t tail = __atomic_load_n(&data_->param_tail, __ATOMIC_ACQUIRE);
    if (head - tail >= VSTPOC_PARAM_RING_MSGS) {
        return;  // ring full; drop
    }
    data_->params[head & (VSTPOC_PARAM_RING_MSGS - 1)] = {index, value};
    __atomic_store_n(&data_->param_head, head + 1, __ATOMIC_RELEASE);
}

void SharedRing::signalStop() {
    if (!data_) return;
    __atomic_store_n(&data_->stop_flag, 1, __ATOMIC_RELEASE);
}

bool SharedRing::guestReady() const {
    if (!data_) return false;
    return __atomic_load_n(&data_->guest_ready, __ATOMIC_ACQUIRE) != 0;
}

uint64_t SharedRing::guestFramesProduced() const {
    if (!data_) return 0;
    return __atomic_load_n(&data_->guest_frames_produced, __ATOMIC_RELAXED);
}
