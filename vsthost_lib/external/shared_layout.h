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

    /* --- Health / diagnostics (added 2026-05-28) ---------------------
     * All fields are appended at the END of the struct to preserve
     * binary layout for older guest builds that don't write them — those
     * builds simply leave the trailing bytes at whatever the host
     * zero-initialised them to. New host reads should be tolerant of
     * fields reading 0 (= "not reported"). Use the diagnostic_layout_v
     * sentinel BELOW to detect a guest that pre-dates these fields
     * (writes 0) vs. one that writes ok-but-still-zero values. */
    uint32_t diagnostic_layout_v;     /* guest writes 1 when it knows the
                                        * fields below exist; 0 = legacy
                                        * guest, fields below meaningless */

    /* DXVK init status:
     *   0 = not attempted (no D3D11 used by plugin)
     *   1 = ok (vkCreateDevice succeeded, no memory alloc failures observed)
     *   2 = memory_alloc_fail (DxvkMemoryAllocator returned null at least
     *       once after the device was created — D3D11 may be partially
     *       broken; see last_memory_alloc_failed_*)
     *   3 = create_device_fail (vkCreateDevice returned non-success;
     *       D3D11InternalCreateDevice will have logged the error)
     *   4 = other failure (catch-all for ext errors) */
    uint32_t dxvk_init_status;

    /* D3D11 device status (separate from DXVK init because DXVK can be
     * loaded without ever creating a D3D11 device — e.g. plugins that
     * use DXGI for output enumeration only):
     *   0 = not_created (D3D11CreateDevice never called)
     *   1 = ok
     *   2 = failed */
    uint32_t d3d11_device_status;

    /* Bitmask of rendering APIs the plugin actually loaded:
     *   bit 0 = D3D11 (loaded d3d11.dll)
     *   bit 1 = D3D9  (loaded d3d9.dll)
     *   bit 2 = OpenGL (loaded opengl32.dll)
     *   bit 3 = GDI (loaded gdi32.dll AND issued BitBlt — passive load
     *           of gdi32 by combase etc. doesn't count)
     *   bit 4 = "none observed yet" (legacy default)
     * Black-screen detector: a plugin with bit 4 set AND
     * wm_user_storm_per_second > 100 AND paint_request_count == 0 is
     * almost certainly stuck in a JUCE event loop without rendering. */
    uint32_t render_api_used;

    /* DXVK memory allocation failure — most-recent attempt that returned
     * null. Set by patch 0003 in dxvk_memory.cpp. */
    uint64_t last_memory_alloc_failed_size;     /* VkMemoryRequirements::size */
    uint32_t last_memory_alloc_failed_types;    /* memoryTypeBits */
    uint32_t last_memory_alloc_failed_count;    /* monotonic; 0 = never */

    /* Render activity counters. paint_request_count covers BOTH WM_PAINT
     * messages dispatched to the editor hwnd AND X11 PutImage requests
     * (the Android-side X11 server counts these and writes via host JNI).
     * Used by the black-screen detector. */
    uint64_t paint_request_count;
    uint64_t wm_paint_count;

    /* VEH catalog hit bitmask — bit N = pattern N in g_veh_patterns hit
     * at least once. Capped at 64 patterns; if we exceed that the bit
     * becomes a tombstone "patterns N..64 collapsed". */
    uint64_t veh_patterns_hit_bitmask;

    /* JUCE WM_USER+123 storm rate, rolling 1s window. Plugins like TH-U
     * legitimately use this for timer ticks (~30/s); >5000/s is a
     * runaway storm that almost always coincides with a stuck editor
     * thread. */
    uint32_t wm_user_storm_per_second;

    /* Free-text written by the guest when it detects an anomaly itself
     * (matches load_status[256]/status_message[256] pair pattern). Set
     * once; subsequent anomalies overwrite. */
    char diagnostic_summary[256];
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
