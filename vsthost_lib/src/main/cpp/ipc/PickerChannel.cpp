#include "PickerChannel.h"
#include "../util/log.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

PickerChannel::PickerChannel(const std::string& path) : path_(path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd_ < 0) {
        LOGE("PickerChannel: open(%s) failed: %s", path.c_str(), std::strerror(errno));
        return;
    }
    if (::ftruncate(fd_, sizeof(VstpocPickerChannel)) != 0) {
        LOGE("PickerChannel: ftruncate failed: %s", std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return;
    }
    void* p = ::mmap(nullptr, sizeof(VstpocPickerChannel),
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED) {
        LOGE("PickerChannel: mmap failed: %s", std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return;
    }
    data_ = static_cast<VstpocPickerChannel*>(p);
    std::memset(data_, 0, sizeof(VstpocPickerChannel));
    LOGI("PickerChannel: mapped %s (%zu bytes)", path.c_str(), sizeof(VstpocPickerChannel));
}

PickerChannel::~PickerChannel() {
    if (data_) ::munmap(data_, sizeof(VstpocPickerChannel));
    if (fd_ >= 0) ::close(fd_);
}

bool PickerChannel::hasRequest(uint32_t* outSeq) const {
    if (!data_) return false;
    const uint32_t req = __atomic_load_n(&data_->request_seq, __ATOMIC_ACQUIRE);
    const uint32_t rsp = __atomic_load_n(&data_->response_seq, __ATOMIC_RELAXED);
    if (outSeq) *outSeq = req;
    return req != rsp;
}

void PickerChannel::readRequest(char* title, char* filter, char* initialDir) const {
    if (!data_) {
        if (title)      title[0]      = '\0';
        if (filter)     filter[0]     = '\0';
        if (initialDir) initialDir[0] = '\0';
        return;
    }
    if (title) {
        std::memcpy(title, data_->request_title, VSTPOC_PICKER_TITLE_LEN);
        title[VSTPOC_PICKER_TITLE_LEN - 1] = '\0';
    }
    if (filter) {
        /* Filter is double-NUL terminated; copy the whole thing including
         * the internal NULs so the Kotlin side can parse alternating
         * description/pattern pairs. */
        std::memcpy(filter, data_->request_filter, VSTPOC_PICKER_FILTER_LEN);
    }
    if (initialDir) {
        std::memcpy(initialDir, data_->request_initial_dir, VSTPOC_PICKER_PATH_LEN);
        initialDir[VSTPOC_PICKER_PATH_LEN - 1] = '\0';
    }
}

void PickerChannel::writeResponse(uint32_t reqSeq, bool cancelled, const char* pathWindows) {
    if (!data_) return;
    if (pathWindows && !cancelled) {
        std::strncpy(data_->response_path, pathWindows, VSTPOC_PICKER_PATH_LEN - 1);
        data_->response_path[VSTPOC_PICKER_PATH_LEN - 1] = '\0';
    } else {
        data_->response_path[0] = '\0';
    }
    data_->response_cancelled = cancelled ? 1 : 0;
    /* Bump response_seq LAST with release ordering so the wine side, when
     * it observes response_seq == request_seq, also sees the filled-in
     * path. */
    __atomic_store_n(&data_->response_seq, reqSeq, __ATOMIC_RELEASE);
}
