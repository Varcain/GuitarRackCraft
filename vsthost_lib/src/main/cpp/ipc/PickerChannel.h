#pragma once

#include <cstdint>
#include <string>

extern "C" {
#include "../../../../external/shared_layout.h"
}

/* Host-side accessor for the file-picker shared-memory channel between
 * wine's patched comdlg32 (writes request) and Android's SAF listener
 * (writes response). One file per plugin process, alongside the audio
 * shared mem.
 *
 * Same lifetime as SharedRing: created when WineHostProcess starts, torn
 * down when the plugin exits. The wine side opens the same file via the
 * VSTPOC_PICKER_PATH env var. */
class PickerChannel {
public:
    explicit PickerChannel(const std::string& path);
    ~PickerChannel();

    PickerChannel(const PickerChannel&) = delete;
    PickerChannel& operator=(const PickerChannel&) = delete;

    bool valid() const { return data_ != nullptr; }
    const std::string& path() const { return path_; }

    /* Has a new request landed that we haven't answered yet? Cheap acquire
     * load on the request_seq; returns its value via outSeq if non-null. */
    bool hasRequest(uint32_t* outSeq) const;

    /* Snapshot the request fields into the provided buffers. Caller must
     * size the destination buffers at least VSTPOC_PICKER_*_LEN bytes. */
    void readRequest(char* title, char* filter, char* initialDir) const;

    /* Submit the response. `pathWindows` is the C:\... path the plugin
     * will receive; pass nullptr or empty + cancelled=true to indicate
     * cancellation. After this call, request_seq == response_seq again
     * and wine wakes up. */
    void writeResponse(uint32_t reqSeq, bool cancelled, const char* pathWindows);

private:
    std::string          path_;
    int                  fd_   = -1;
    VstpocPickerChannel* data_ = nullptr;
};
