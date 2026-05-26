// Cross-process shared-memory layout for the M4 IPC channel between the
// ARM-native host (Oboe RT thread) and the x86_64 guest running under Box64.
//
// Included by BOTH:
//   - app/src/main/cpp/ipc/SharedRing.{h,cpp}      (ARM aarch64, C++)
//   - external/guest/guest.c                        (x86_64, freestanding C)
//
// Keep this file pure C / stdint so both sides see the same layout.
// All atomics are 64-bit on a 64-byte cache line; offsets are stable across
// the two ABIs because both x86_64 and aarch64 use 8-byte alignment for
// uint64_t and the same struct-padding rules under -fno-pic in the guest
// and standard C++ on the host.

#ifndef VSTPOC_SHARED_LAYOUT_H
#define VSTPOC_SHARED_LAYOUT_H

#include <stdint.h>

#define VSTPOC_AUDIO_RING_FRAMES  16384   /* power of 2; ~340 ms at 48 kHz stereo */
#define VSTPOC_PARAM_RING_MSGS     64     /* power of 2 */
#define VSTPOC_CHANNELS            2
#define VSTPOC_CACHELINE           64
#define VSTPOC_MAX_PARAMS          128    /* bumped from 8 — Lecto and other deeper plugins expose 20+ params */
#define VSTPOC_PARAM_NAME_LEN      32     /* per-name buffer including NUL */

/* Native file-picker channel sizes. Wine-side GetOpenFileNameA hook writes
 * the request, Android-side SAF listener writes the response. */
#define VSTPOC_PICKER_TITLE_LEN     128
#define VSTPOC_PICKER_FILTER_LEN    512   /* Win32 filter spec: "Wave\0*.wav\0..." double-NUL ended */
#define VSTPOC_PICKER_PATH_LEN     1024

typedef struct {
    int32_t index;
    float   value;
} VstpocParamMsg;

/* Single mmap region. Atomics laid out one-per-cacheline so producer and
 * consumer never share a line (avoids false sharing during heavy churn). */
typedef struct {
    /* control */
    _Alignas(VSTPOC_CACHELINE) uint64_t stop_flag;          /* host→guest: set 1 to ask guest to exit */
    _Alignas(VSTPOC_CACHELINE) uint64_t guest_ready;        /* guest→host: set 1 after mmap+init */
    _Alignas(VSTPOC_CACHELINE) uint64_t guest_frames_produced; /* monotonic counter for diagnostics */
    _Alignas(VSTPOC_CACHELINE) uint64_t mic_active;         /* host→guest: 1 = consume audio_in instead of generating */

    /* audio out ring: guest is producer, host is consumer */
    _Alignas(VSTPOC_CACHELINE) uint64_t audio_head;
    _Alignas(VSTPOC_CACHELINE) uint64_t audio_tail;

    /* audio in ring: host is producer (mic callback), guest is consumer */
    _Alignas(VSTPOC_CACHELINE) uint64_t audio_in_head;
    _Alignas(VSTPOC_CACHELINE) uint64_t audio_in_tail;

    /* param ring: host is producer, guest is consumer */
    _Alignas(VSTPOC_CACHELINE) uint64_t param_head;
    _Alignas(VSTPOC_CACHELINE) uint64_t param_tail;

    /* payload arrays, cache-line aligned so they don't share with the above */
    _Alignas(VSTPOC_CACHELINE) float          audio[VSTPOC_AUDIO_RING_FRAMES * VSTPOC_CHANNELS];
    _Alignas(VSTPOC_CACHELINE) float          audio_in[VSTPOC_AUDIO_RING_FRAMES * VSTPOC_CHANNELS];
    _Alignas(VSTPOC_CACHELINE) VstpocParamMsg params[VSTPOC_PARAM_RING_MSGS];

    /* metadata: written once by the guest at startup, BEFORE setting
     * guest_ready. The host reads after seeing guest_ready=1. */
    int32_t param_count;
    char    param_names[VSTPOC_MAX_PARAMS][VSTPOC_PARAM_NAME_LEN];

    /* plugin load status. Written by guest BEFORE it exits (success or
     * failure path). The host polls this so it can surface load errors
     * to the UI instead of silently showing a blank editor.
     *   0 = pending (guest still trying)
     *   1 = ok (set together with guest_ready=1)
     *   2 = failed — see status_message for reason */
    int32_t load_status;
    char    status_message[256];

    /* Plugin editor preferred size, in plugin-native pixels. Written by
     * vst_host after effEditGetRect succeeds; 0/0 if no editor or not yet
     * known. The Android side reads this to size the SurfaceView so the
     * plugin's GUI maps 1:1 in aspect (no letterbox, touch coords clean). */
    int32_t editor_width;
    int32_t editor_height;
} VstpocShared;

/* Native file-picker channel — lives in its OWN mmap file
 * (vst_picker_pN.dat next to vst_shm_pN.dat) so wine's comdlg32 hook
 * doesn't need to know the layout of VstpocShared.
 *
 * Wine flow (when env VSTPOC_PICKER_PATH is set + comdlg32 patch is
 * active): GetOpenFileNameA/W writes request_title / request_filter /
 * request_initial_dir, then bumps `request_seq` from N to N+1. It then
 * polls `response_seq` until it equals request_seq, sleeping ~25 ms.
 *
 * Android flow: a coroutine on the Compose side polls request_seq vs
 * response_seq. When request_seq is ahead, it parses the Win32 filter
 * into MIME types, launches ACTION_OPEN_DOCUMENT, copies the picked URI
 * into wineprefix/drive_c/users/<u>/Documents/IRs/<name>, fills
 * response_path with the resulting Windows path (or leaves it empty +
 * sets response_cancelled=1), then bumps response_seq to match.
 *
 * Multi-plugin: each plugin process has its own picker file (p0, p1,
 * …); the Android listener multiplexes across them. */
typedef struct {
    _Alignas(VSTPOC_CACHELINE) volatile uint32_t request_seq;
    _Alignas(VSTPOC_CACHELINE) volatile uint32_t response_seq;
    int32_t response_cancelled;     /* 1 = user cancelled, response_path may be empty */
    int32_t reserved0;
    char    request_title       [VSTPOC_PICKER_TITLE_LEN];
    char    request_filter      [VSTPOC_PICKER_FILTER_LEN];
    char    request_initial_dir [VSTPOC_PICKER_PATH_LEN];
    char    response_path       [VSTPOC_PICKER_PATH_LEN];
} VstpocPickerChannel;

#define VSTPOC_PARAM_INDEX_GAIN 0
#define VSTPOC_PARAM_INDEX_FREQ 1

#endif /* VSTPOC_SHARED_LAYOUT_H */
