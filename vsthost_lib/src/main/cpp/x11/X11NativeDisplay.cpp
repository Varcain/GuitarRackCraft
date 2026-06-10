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

#include "X11NativeDisplay.h"
#include "X11Protocol.h"
#include "X11ByteOrder.h"
#include "X11AtomStore.h"
#include "X11WindowManager.h"
#include "X11PixmapStore.h"
#include "X11Framebuffer.h"
#include "X11ConnectionHandler.h"
#include "X11EventBuilder.h"
#include "X11Log.h"
#include "../plugin/PluginUIGuard.h"

#include "../utils/ThreadUtils.h"
#include <android/log.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/un.h>
#include <chrono>
#include <cstring>
#include <thread>
#include <atomic>
#include <future>
#include <vector>
#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include <unordered_map>
#include <unordered_set>
#include <cerrno>
#include <cstdio>
#include <string>
#include <algorithm>
#include <climits>

#define LOG_TAG "X11NativeDisplay"
#define LOGI(...) X11_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) X11_LOGE(LOG_TAG, __VA_ARGS__)
#define LOGW(...) X11_LOGW(LOG_TAG, __VA_ARGS__)

/* Swap R and B channels in an array of ARGB pixels using NEON SIMD.
 * Each pixel: swap byte 0 (B/R) with byte 2 (R/B), keep bytes 1 (G) and 3 (A). */
static void swapRB_neon(const uint32_t* __restrict src, uint32_t* __restrict dst, size_t count) {
#if defined(__aarch64__) || defined(__ARM_NEON)
    size_t i = 0;
    /* Process 4 pixels (16 bytes) at a time using byte-level table lookup */
    for (; i + 4 <= count; i += 4) {
        uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(src + i));
        /* Rearrange bytes: for each pixel [B,G,R,A] → [R,G,B,A] (swap 0↔2) */
        static const uint8_t tbl[16] = {2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15};
        uint8x16_t idx = vld1q_u8(tbl);
        uint8x16_t swapped = vqtbl1q_u8(v, idx);
        vst1q_u8(reinterpret_cast<uint8_t*>(dst + i), swapped);
    }
    /* Handle remaining pixels */
    for (; i < count; i++) {
        uint32_t p = src[i];
        dst[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
    }
#else
    for (size_t i = 0; i < count; i++) {
        uint32_t p = src[i];
        dst[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
    }
#endif
}

namespace guitarrackcraft {

static constexpr int kX11BasePort = 6000;
static constexpr double kTouchFlushIntervalSec = 1.0 / 30.0;

// Log up to 64 bytes as hex (16 per line) for debugging connection setup.
static void logHex(const char* label, const uint8_t* data, size_t len) {
    const size_t maxLog = (len < 64) ? len : 64;
    for (size_t i = 0; i < maxLog; i += 16) {
        size_t n = (maxLog - i) < 16 ? (maxLog - i) : 16;
        char line[80];
        int off = 0;
        for (size_t j = 0; j < n; ++j) {
            off += snprintf(line + off, sizeof(line) - (size_t)off, "%02x ", data[i + j]);
        }
        LOGI("%s [+%zu] %s", label, i, line);
    }
}

// Use X11Op::, X11Event::, and k-prefixed constants from X11Protocol.h
using namespace X11Op;
using namespace X11Event;

/* P1 GPU compositor (see memory project_gpu_xserver_upgrade). When true,
 * renderLoop composites the editor layer and each popup as separate textured
 * quads on the GPU in z-order (painter's algorithm) instead of CPU-memcpy'ing
 * popup pixels into the editor framebuffer. This is the structural foundation
 * for AHardwareBuffer-backed per-window textures (DRI3/Present, phases P3/P4):
 * a window's texture source becomes an imported EGLImage instead of a CPU
 * upload, but the compositing draw is identical. The editor layer is still
 * full-uploaded each frame here (no behaviour/cost change vs the monolithic
 * path) — dirty-region upload is P1.1. Flip to false for the proven monolithic
 * path (instant, byte-identical fallback for the 5 working plugins). */
static constexpr bool kGpuCompositor = true;

struct X11NativeDisplay::Impl {
    ANativeWindow* window = nullptr;
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLSurface eglSurface = EGL_NO_SURFACE;
    EGLContext eglContext = EGL_NO_CONTEXT;
    int width = 0;
    int height = 0;
    int displayNumber_ = 0;
    std::vector<uint32_t> framebuffer;  // ARGB (GL format) - main framebuffer for rendering
    // Framebuffer now stores X11 wire format (BGRA) directly — no separate shadow needed
    std::vector<uint32_t> renderBuffer; // Staging buffer for render thread (triple buffering)
    int serverFd = -1;
    /* The actual TCP port the listener got bound on. Normally equals
     * kX11BasePort + displayNumber_ (6001 for display 1, 6002 for display 2,
     * …) but if that port was held by an orphan, serverLoop tries 6101,
     * 6201, … until one is free. WineHostProcess reads this via
     * X11NativeDisplay::getActualPort() so DISPLAY=127.0.0.1:N matches the
     * port the new wine subprocess will actually be talking to. */
    int actualPort_ = -1;
    // clientFd is per-thread: each accepted X connection runs in its own
    // std::thread; that thread's clientFd is its own socket fd. References
    // from outside the connection thread (e.g. teardown from the UI thread)
    // will see -1, which is fine — those code paths gate on it and just
    // skip when no client is "active" from their perspective. Active fds
    // for teardown are tracked separately in activeClientFds.
    static thread_local int clientFd;
    std::mutex activeClientsMutex;
    std::vector<int> activeClientFds;
    std::atomic<bool> running{false};
    bool useUnixSocket_ = false;
    std::atomic<bool> renderThreadRunning{false};  // Separate flag for render thread to allow pause/resume
    std::thread serverThread;
    std::thread renderThread;
    std::mutex bufferMutex;
    std::atomic<bool> dirty{false};
    std::mutex dirtyMutex;                    // Protects dirty condition variable
    std::condition_variable dirtyCv;          // Signals render thread when dirty changes
    std::atomic<bool> detachDeferred{false};  // Set when detach is deferred due to plugin creation
    // Graceful teardown state
    std::atomic<bool> closingGracefully{false};  // Set when graceful teardown initiated
    std::atomic<bool> destroyNotifySent{false};  // Set when DestroyNotify has been sent
    std::chrono::steady_clock::time_point closeStartTime;  // When graceful teardown started
    // Idle callback - called from pluginUI thread to process plugin UI events
    std::function<void()> idleCallback;
    std::mutex idleCallbackMutex;
    // Plugin UI thread: dedicated thread for all plugin Display* access
    std::thread pluginUIThread;
    std::mutex pluginUITaskMutex;
    std::condition_variable pluginUITaskCv;
    std::queue<std::function<void()>> pluginUITasks;
    std::atomic<bool> pluginUIRunning{false};
    // The connection fd that owns the plugin editor window (set by the
    // CreateWindow handler when a top-level window ≥64x64 appears as a
    // child of root). Touch events need to be delivered to THIS fd — wine
    // is multi-connection now and the editor lives on exactly one of them.
    std::atomic<int> editorOwnerFd{-1};

    // Ordered list of top-level plugin editor windows (root-children, ≥64x64),
    // in CreateWindow order. The Nth entry is rendered at framebuffer
    // y-offset = N * pluginHeight, producing a vertical stack of editors
    // (effects-rack layout). Protected by `windowMapMutex` (same mutex that
    // protects windowManager_'s child window list).
    std::vector<uint32_t> pluginSlotWindows;

    /* Override-redirect popup overlays.
     *
     * Wine creates combobox dropdowns, right-click context menus, tooltips,
     * etc. as top-level windows with `override_redirect=1`. These are NOT
     * editors (they shouldn't be slot windows — they have transient, small,
     * arbitrary geometry that overlaps the editor) and the slot framebuffer
     * has nowhere to put them. We capture each popup's pixels into its own
     * backing buffer here and composite them over the framebuffer in
     * renderLoop. Lifecycle:
     *   - CreateWindow with CWOverrideRedirect=1     → insert entry
     *   - ConfigureWindow                            → update x/y/w/h (resize buffer)
     *   - MapWindow / UnmapWindow                    → toggle `mapped`
     *   - PutImage targeting a popup wid             → write into `pixels`
     *   - DestroyWindow                              → erase entry
     * Protected by `bufferMutex` (same mutex that guards the framebuffer
     * + slot lists, so the renderer can snapshot framebuffer + popup state
     * atomically). */
    struct PopupOverlay {
        int x = 0, y = 0;            // displayed position (after smart-flip)
        int w = 0, h = 0;
        // Wine-requested coords — preserved across ConfigureWindow calls that
        // only update a subset of geometry. The smart-flip recomputes from
        // these so each call produces the same flipped output (idempotent),
        // and oscillations from wine re-issuing ConfigureWindow with X+W+H
        // but no Y can't happen.
        int reqX = 0, reqY = 0;
        bool mapped = false;
        /* Set true once wine actually PutImage's pixels into this overlay.
         * Compositor skips popups that are mapped-but-never-painted — wine
         * creates internal IME / helper popups it never displays; they
         * exist in childWindows and pass the ConfigureWindow implicit-map
         * heuristic, but their pixels stay 0xFF000000 (the init fill) and
         * we'd composite a black rectangle over the editor at their pos. */
        bool hasContent = false;
        std::vector<uint32_t> pixels;  // BGRA, packed w * h
    };
    std::unordered_map<uint32_t, PopupOverlay> popupOverlays;

    /* A JUCE menu / combo dropdown is rendered by wine as a real body window
     * (both dimensions comfortably > 32px) PLUS up to four thin drop-shadow
     * decoration strips around it (one dimension ~12-15px, the other the full
     * body length — e.g. 12x332 sides, 304x12 top/bottom). Wine PutImages the
     * shadow gradient into those strips so hasContent is true, but compositing
     * them paints a black/white band around the real menu (the "white area
     * around popups" symptom). A genuine popup body is never this thin in
     * EITHER dimension, so treat any overlay with a sub-32px dimension as a
     * shadow strip and skip it for BOTH rendering and hit-testing (the two must
     * agree or clicks land on an invisible strip). This restores the proven
     * pre-2026-06-09 filter; see feedback_popup_overlay_has_content and
     * feedback_x50_stomp_popup_grab_dismiss. A real popup must also have been
     * PutImage'd (hasContent) — an unpainted overlay is invisible, so a click
     * on it is meaningless and would otherwise route to wine's internal
     * never-displayed helper windows (e.g. the 166x45 IME helper at 0,0). */
    static constexpr int kMinPopupDim = 32;
    static bool isRenderablePopup(const PopupOverlay& p) {
        return p.mapped && p.hasContent &&
               p.w >= kMinPopupDim && p.h >= kMinPopupDim;
    }
    // Mirror used by renderLoop while it holds bufferMutex (we snapshot
    // mapped popups into this list under the lock, then composite outside).
    struct PopupSnapshot {
        uint32_t wid;  // GPU compositor keys a per-popup texture by this
        int x, y, w, h;
        std::vector<uint32_t> pixels;
    };
    // Slot index of the most-recent injectTouch (UI thread → connection
    // thread handoff). drainTouchQueue reads this to route the event to
    // the right plugin window. With one physical touch source, races are
    // negligible since events arrive in order.
    std::atomic<int> lastTouchSlot{0};
    // Render thread exit synchronization
    std::atomic<bool> renderThreadExited{false};
    std::mutex renderExitMutex;
    std::condition_variable renderExitCv;
    GLuint program = 0;
    GLuint texUniform = 0;
    GLuint fbTex = 0;    // persistent framebuffer (editor-layer) texture
    GLuint fbVbo = 0;    // per-quad vertex buffer (DYNAMIC, reused per draw)
    /* GPU compositor (kGpuCompositor): one GL texture per popup overlay, keyed
     * by window id. Touched only by the render thread (the one with the GL
     * context). Textures are sized via glTexImage2D on upload, so a recycled
     * wid with a new size is handled correctly. Bounded (popup wids per
     * session) and leaked at context teardown — we intentionally skip EGL
     * teardown, so an explicit GC buys nothing. */
    std::unordered_map<uint32_t, GLuint> popupTextures_;
    X11ByteOrder byteOrder_{true};  // X11 byte order (replaces msbFirst_)
    // Convenience aliases: keep existing call sites working via delegation
    bool& msbFirst_ = byteOrder_.msbFirst;
    std::atomic<bool> listening_{false};
    std::atomic<uint16_t> lastSeq_{0};  // last request sequence number, for event injection
    /* Per-connection sequence tracker. XCB demands events be stamped with
     * the seq number of the *target client's* last request — a single
     * shared atomic confuses xcb's threaded poll loop when multiple wine
     * processes share the display (xcb_xlib_threads_sequence_lost). Each
     * connection thread maintains its own value. */
    static thread_local uint16_t lastReplySeq_;
    /* Per-fd snapshot of the most recent reply sequence number sent on
     * that connection. Used by sendEvent when the event target window is
     * owned by a different connection than the current thread — synthetic
     * events must carry the TARGET connection's seq so its libX11 stays
     * in sync, not the current thread's. Updated atomically in sendReply
     * (which knows the current thread's fd). */
    std::mutex fdSeqMutex;
    std::unordered_map<int, uint16_t> fdLastSeq;

    /* vstpoc 2026-05-24: per-fd write mutex (was: one global writeMutex).
     * The old single mutex serialized ALL outbound traffic across every
     * client connection. If wine thread A's recv buffer fills (because A
     * is stuck mid-JUCE-destructor and not reading), our blocking send()
     * to A holds the global mutex; every other thread's send to its OWN
     * fd queues behind it. Result: one stuck wine thread starves all X11
     * traffic for every other thread, magnifying the popup-close deadlock.
     *
     * Per-fd mutex isolates per-connection writes. writeMapMutex protects
     * only the lookup (microseconds); the per-fd mutex protects the
     * actual send() (potentially blocking). Map entries are never erased
     * — bounded by number of distinct connection fds (typically <50;
     * each mutex is ~40 bytes), avoids any lifetime races with concurrent
     * lookups. */
    std::mutex writeMapMutex;
    std::unordered_map<int, std::unique_ptr<std::mutex>> perFdWriteMutex;

    /* vstpoc: per-fd OUTPUT QUEUE. A real X server never drops events; on a
     * non-blocking socket whose send buffer is full, the old sendAll() returned
     * false and the event (MapNotify/PropertyNotify/reply) was LOST — breaking
     * wine's window state machine so it re-asserts forever (storm). Instead we
     * buffer the un-sent bytes here and flush them opportunistically (in the
     * read-loop poll and before the next send). Never blocks the read loop,
     * never drops. Guarded by the per-fd write mutex. */
    std::unordered_map<int, std::vector<uint8_t>> outQueue_;
    static constexpr size_t kMaxOutQueue = 32 * 1024 * 1024; /* 32MB; past this the client is hopelessly stuck → drop connection */
    std::atomic<int> lastPointerX{0};     // last injected pointer X position (for QueryPointer)
    std::atomic<int> lastPointerY{0};     // last injected pointer Y position (for QueryPointer)
    std::atomic<bool> pointerButton1Down{false};  // button 1 state (for QueryPointer)
    std::atomic<bool> pointerButton3Down{false};  // button 3 state (for QueryPointer)

    // Extracted modules (replace inline state)
    X11AtomStore atoms_;
    X11WindowManager windowManager_{kRootWindowId};
    X11PixmapStore pixmapStore_;
    X11EventBuilder eventBuilder_{byteOrder_};

    /* WM_STATE property per window. Wine's can_activate_window checks
     * data->current_state.wm_state != WithdrawnState before allowing
     * NtUserSetForegroundWindow on focus events. current_state.wm_state
     * is updated from a WM_STATE PropertyNotify (wine's
     * handle_wm_state_notify reads the property via XGetWindowProperty).
     * On a real X11 server the window manager sets this property after
     * XMapWindow; we have no WM so we set it ourselves and emit
     * PropertyNotify in MapWindow / UnmapWindow handlers. Reading is
     * routed through GetProperty (case 20). */
    std::mutex wmStateMutex;
    std::unordered_map<uint32_t, uint32_t> wmStateValues;


    /* Generic X11 property store. Wine 11.9's window-state machine WRITES a
     * property via ChangeProperty, then READS IT BACK via GetProperty (e.g.
     * window_wm_normal_hints_notify → XGetWMNormalHints, get_window_mwm_hints)
     * and compares the read-back to what it requested; handle_state_change
     * (window.c:1840) DROPS the confirmation on any value mismatch, so the
     * window state never converges and the editor host window never becomes
     * ready ("Loading editor…" forever — a 10.10→11.9 regression). Before this
     * store, GetProperty returned empty for everything except WM_STATE, so
     * every hint read-back mismatched. We now echo back exactly what was
     * written. Key = (window<<32)|atom. */
    struct PropertyValue { uint32_t type = 0; uint8_t format = 0; std::vector<uint8_t> data; };
    std::mutex propStoreMutex;
    std::unordered_map<uint64_t, PropertyValue> propStore_;

    /* GC foreground color per GC ID. Tracked so PolyFillRectangle can
     * fill with the right color — Win32 EDIT controls (and many other
     * widgets) call FillRect(GC,fg=white) for their background, which
     * translates to X11 ChangeGC(fg=0xFFFFFFFF) + PolyFillRectangle. We
     * used to no-op PolyFillRectangle; the result was that any widget
     * that relies on FillRect (test_focus EDIT, plugin headers, etc.)
     * rendered as opaque black (whatever the overlay was initialized
     * to) instead of its intended background color. */
    std::mutex gcMutex;
    std::unordered_map<uint32_t, uint32_t> gcForeground;

    // Legacy accessors — delegate to windowManager_ for code that still uses these directly
    const std::vector<uint32_t>& childWindows = windowManager_.childWindows();

    /* Queued touch events: injected from UI thread, drained from server thread between requests.
     * This prevents xcb_xlib_threads_sequence_lost: events must not arrive on the socket while
     * the client-side libX11/XCB is in the middle of a request/reply exchange. */
    struct QueuedTouch {
        int action; // 0=down, 1=up, 2=move, 3=right-click tap (button 3 press+release)
        int x, y;
    };
    std::mutex touchQueueMutex;
    std::vector<QueuedTouch> touchQueue;
    /* Queued key events. Same drain pattern as QueuedTouch. action 0 = press,
     * 1 = release. keycode is the X11 hardware keycode that maps (via our
     * kJavaKeymapPayload reply) to the desired keysym. state is the X11
     * modifier mask (Shift = 0x01). */
    struct QueuedKey { int action; uint8_t keycode; uint16_t state; };
    std::mutex keyQueueMutex;
    std::vector<QueuedKey> keyQueue;
    /* Wine SetInputFocus target. Updated by the SetInputFocus handler;
     * used for diagnosis (keyboard routing currently uses grabWindow,
     * the last-clicked widget). Not load-bearing. */
    std::atomic<uint32_t> focusedWindowId{0};
    /* DEBUG focus-emulation A/B switch (BIAS FX 2 CEF caret bisection). Bitmask
     * read from <appCache>/x11_focus_mode.txt, re-read at startServer + on every
     * touch-down, so focus-emulation variants can be toggled on-device WITHOUT
     * rebuilding the APK (write the file via `toybox tee`, then tap). 0 = current
     * behavior. Bits:
     *   0x1  skip synthetic WM_TAKE_FOCUS in click-to-focus
     *   0x2  skip synthetic FocusOut/FocusIn in click-to-focus
     *   0x4  skip FocusOut/FocusIn echo in SetInputFocus (case 42)
     *   0x8  GetInputFocus returns PointerRoot instead of focusedWindowId
     *   0x10 legacy only: click-to-focus skips bestKeyTarget==0 by default now
     *   0x20 skip click-to-focus entirely (no focus events on click at all)
     *   0x40 send a hover MotionNotify to the hit window BEFORE the ButtonPress
     *   0x80 send an EnterNotify to the hit window BEFORE the ButtonPress */
    std::atomic<uint32_t> focusModeMask_{0};
    uint32_t readFocusMode() {
        static std::string fmPath;
        if (fmPath.empty()) {
            std::string pkg;
            if (FILE* c = fopen("/proc/self/cmdline", "r")) {
                char b[256] = {0}; (void)fread(b, 1, sizeof(b) - 1, c); fclose(c);
                pkg = b;
                auto colon = pkg.find(':');
                if (colon != std::string::npos) pkg.resize(colon);
            }
            if (pkg.empty()) pkg = "com.varcain.guitarrackcraft";
            fmPath = "/data/data/" + pkg + "/cache/x11_focus_mode.txt";
        }
        uint32_t v = 0;
        if (FILE* f = fopen(fmPath.c_str(), "r")) { if (fscanf(f, "%u", &v) != 1) v = 0; fclose(f); }
        return v;
    }
    /* Maps each X11 window id to the fd of the client that created it.
     * Touch routing uses this to find the right destination even if the
     * client that did the drawing (and was originally claimed as owner)
     * has disconnected. */
    std::mutex windowCreatorMutex;
    std::unordered_map<uint32_t, int> windowCreator;
    std::mutex windowMapMutex;  // protects windowManager_ reads from UI thread via isWidgetAtPoint
    int pluginWidth = 0;   // plugin's current window width (may be scaled)
    int pluginHeight = 0;  // plugin's current window height
    float uiScale = 1.0f;  // UI scale factor for plugin rendering (< 1.0 = smaller = faster)
    /** When true, automatic slot-promotion paths (CreateWindow handler
     *  + PutImage promotion) skip framebuffer resize. Set by the
     *  installer flow before launching wine: an installer wizard may
     *  create a small initial dialog (~330×277) then immediately
     *  reposition it inside the larger desktop coords like (507, 445)
     *  — if we shrunk the framebuffer to match the initial CreateWindow
     *  size, those subsequent paints land off-screen. Plugins always
     *  paint inside their own window so this flag stays false for them. */
    bool fbSizeFrozen = false;
    /* Auto-crop region used in installer mode (fbSizeFrozen=true). The
     * wizard window (InnoSetup/NSIS) tends to be much smaller than the
     * frozen 1024×768 framebuffer — e.g. 330×277 centered. Rendering the
     * whole framebuffer letterboxed makes the wizard appear tiny.
     *
     * Tracked from ConfigureWindow on the largest mapped non-popup
     * root-child window when fbSizeFrozen. The renderer stride-copies
     * this sub-rect of the framebuffer into renderBuffer at size cropW
     * × cropH, then letterboxes that. injectTouch inverse-maps through
     * the same rect so surface taps land on the wizard's coords.
     *
     * 0 sentinel = no crop (use full framebuffer). Protected by
     * bufferMutex (read in renderLoop, written in ConfigureWindow). */
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    uint32_t cropWid = 0;  // wid of the window the crop rect tracks (for popup-buffer source)
    // Bounding box of pixels actually written by PutImage into the active slot
    // window. Differs from pluginWidth/Height when the plugin's CreateWindow
    // size doesn't match what its renderer actually paints — e.g., AmpCraft
    // declares 1290x612 via effEditGetRect but DXVK presents only 896 wide
    // (wine StretchBlt downscales for DPI). Compose polls this to size the
    // SurfaceView to the rendered region instead of the declared size, so
    // the right-side brown band disappears.
    int renderedMaxX_ = 0;
    int renderedMaxY_ = 0;
    uint32_t renderedExtentSlot_ = 0;  // which wid we're tracking; reset on change

    // Compute absolute position — delegates to windowManager_
    std::pair<int, int> getAbsolutePos(uint32_t wid) const {
        return windowManager_.getAbsolutePos(wid);
    }

    // Byte-order read/write — delegate to byteOrder_ (keeps all existing call sites working)
    uint16_t read16(const uint8_t* p, int off) const { return byteOrder_.read16(p, off); }
    uint32_t read32(const uint8_t* p, int off) const { return byteOrder_.read32(p, off); }
    void write16(uint8_t* p, int off, uint16_t val) const { byteOrder_.write16(p, off, val); }
    void write32(uint8_t* p, int off, uint32_t val) const { byteOrder_.write32(p, off, val); }

    bool initGL() {
        const char* vs = "attribute vec2 aPos; attribute vec2 aTex; varying vec2 vTex; void main() { gl_Position = vec4(aPos, 0, 1); vTex = aTex; }";
        const char* fs = "precision mediump float; varying vec2 vTex; uniform sampler2D uTex; void main() { vec4 c = texture2D(uTex, vTex); gl_FragColor = vec4(c.b, c.g, c.r, 1.0); }";
        GLuint vsId = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vsId, 1, &vs, nullptr);
        glCompileShader(vsId);
        GLuint fsId = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fsId, 1, &fs, nullptr);
        glCompileShader(fsId);
        program = glCreateProgram();
        glAttachShader(program, vsId);
        glAttachShader(program, fsId);
        glLinkProgram(program);
        glDeleteShader(vsId);
        glDeleteShader(fsId);
        texUniform = glGetUniformLocation(program, "uTex");
        glGenTextures(1, &fbTex);
        glGenBuffers(1, &fbVbo);
        float verts[] = { -1,-1, 0,1,  1,-1, 1,1,  -1,1, 0,0,  1,1, 1,0 };
        glBindBuffer(GL_ARRAY_BUFFER, fbVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        return program != 0;
    }

    /* GPU compositor helpers (kGpuCompositor). Render-thread only. */

    // (Re)upload a BGRA-wire-format CPU buffer into a GL texture. glTexImage2D
    // re-specifies size each call, so a texture reused across differently-sized
    // frames/popups stays correct. The fragment shader does the BGRA->RGBA
    // swizzle, matching the monolithic path.
    void uploadTex(GLuint tex, const uint32_t* pixels, int w, int h) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    // Draw `tex` as a quad covering the root-relative pixel rect (px,py,pw,ph),
    // mapped through the active letterbox transform (origin x0,y0 + uniform
    // `scale`) into the surfW x surfH output. Texture row 0 maps to the top of
    // the rect (v-flip), identical to the monolithic path's static quad.
    // Assumes glUseProgram(program) + glViewport(0,0,surfW,surfH) already set.
    void compositeQuad(GLuint tex, int px, int py, int pw, int ph,
                       int surfW, int surfH, int x0, int y0, float scale) {
        float sx0 = x0 + px * scale, sy0 = y0 + py * scale;
        float sx1 = sx0 + pw * scale, sy1 = sy0 + ph * scale;
        float nx0 = sx0 / surfW * 2.f - 1.f;
        float nx1 = sx1 / surfW * 2.f - 1.f;
        float nyTop = 1.f - sy0 / surfH * 2.f;  // rect top edge (image row 0)
        float nyBot = 1.f - sy1 / surfH * 2.f;  // rect bottom edge
        const float verts[] = {
            nx0, nyBot, 0.f, 1.f,   // bottom-left  -> tex (0,1)
            nx1, nyBot, 1.f, 1.f,   // bottom-right -> tex (1,1)
            nx0, nyTop, 0.f, 0.f,   // top-left     -> tex (0,0)
            nx1, nyTop, 1.f, 0.f,   // top-right    -> tex (1,0)
        };
        glBindBuffer(GL_ARRAY_BUFFER, fbVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
        GLint aPos = glGetAttribLocation(program, "aPos");
        GLint aTex = glGetAttribLocation(program, "aTex");
        glEnableVertexAttribArray((GLuint)aPos);
        glVertexAttribPointer((GLuint)aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray((GLuint)aTex);
        glVertexAttribPointer((GLuint)aTex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(texUniform, 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    void renderLoop() {
        LOGI("X11Debug: render thread STARTED display=%d tid=%ld", displayNumber_, getTid());
        bool glInited = false;
        int frameCount = 0;

        while (renderThreadRunning) {
            // Wait for dirty flag using condition variable (eliminates busy-waiting)
            {
                std::unique_lock<std::mutex> lock(dirtyMutex);
                dirtyCv.wait(lock, [this] {
                    return dirty.load() || !renderThreadRunning.load();
                });
            }

            if (!renderThreadRunning) break;
            if (!dirty || eglSurface == EGL_NO_SURFACE) {
                continue;
            }

            frameCount++;
            if (frameCount <= 5 || frameCount % 60 == 0) {
                LOGI("X11Debug: render thread display=%d frame #%d dirty=%d eglSurface=%p",
                     displayNumber_, frameCount, dirty.load() ? 1 : 0, (void*)eglSurface);
            }
            if (eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext) != EGL_TRUE) {
                usleep(5000);
                continue;
            }
            if (!glInited) {
                glInited = initGL();
                if (!glInited) {
                    dirty = false;
                    continue;
                }
            }
            // Copy framebuffer to staging buffer under lock, then clear dirty BEFORE rendering.
            // Clearing dirty here (not after swap) prevents a race where PutImage sets dirty=true
            // during GL render/swap, only to have it clobbered by a post-swap dirty=false.
            int fw = 0, fh = 0;
            int cropOffX = 0, cropOffY = 0;
            std::vector<PopupSnapshot> popupSnapshots;
            {
                std::lock_guard<std::mutex> lock(bufferMutex);
                fw = pluginWidth > 0 ? pluginWidth : width;
                fh = pluginHeight > 0 ? pluginHeight : height;
                // Stacked-editor render: framebuffer height = pluginHeight * slot_count.
                // We render the WHOLE stack (letterboxed) into the SurfaceView.
                if (!pluginSlotWindows.empty()) {
                    fh = pluginHeight * (int)pluginSlotWindows.size();
                }
                bool useCrop = false;  // installer auto-crop disabled — see ConfigureWindow note
                if (!useCrop && width > 0 && height > 0 && !framebuffer.empty()) {
                    if (renderBuffer.size() != framebuffer.size()) {
                        renderBuffer.resize(framebuffer.size());
                    }
                    renderBuffer = framebuffer;  // Copy to staging buffer
                }
                /* Snapshot mapped popup overlays IN X11 STACKING ORDER so
                 * sub-popups (created after their parent) render ON TOP of
                 * the parent. windowManager_'s childWindows tracks insertion
                 * order (oldest first); we iterate it and pick up only
                 * popup-tagged wids that are currently mapped.
                 *
                 * isRenderablePopup() filters wine's thin drop-shadow strips
                 * (the "white area around popups" band) and unpainted internal
                 * helper windows; real menu/combo/dialog bodies pass. */
                {
                    std::lock_guard<std::mutex> mapLock(windowMapMutex);
                    for (uint32_t wid : windowManager_.childWindows()) {
                        if (useCrop && wid == cropWid) continue;
                        auto it = popupOverlays.find(wid);
                        if (it == popupOverlays.end()) continue;
                        const auto& p = it->second;
                        if (!isRenderablePopup(p)) continue;
                        if (p.pixels.size() != (size_t)p.w * p.h) continue;
                        if (useCrop) {
                            popupSnapshots.push_back({wid,
                                                      p.x - cropOffX,
                                                      p.y - cropOffY,
                                                      p.w, p.h, p.pixels});
                        } else {
                            popupSnapshots.push_back({wid, p.x, p.y, p.w, p.h, p.pixels});
                        }
                    }
                }
            }
            /* Composite popups onto the staging buffer at their root-relative
             * coordinates, clipped to the framebuffer. Opaque blit — popups
             * in our use case (combo dropdowns, context menus, tooltips) are
             * always opaque and override any pixels beneath them.
             * GPU-compositor path draws popups as their own quads instead
             * (see the kGpuCompositor render block below), so skip the CPU
             * memcpy here when it's active. */
            if (!kGpuCompositor &&
                !popupSnapshots.empty() && fw > 0 && fh > 0 && !renderBuffer.empty()) {
                for (const auto& p : popupSnapshots) {
                    int dstX0 = p.x;
                    int dstY0 = p.y;
                    int srcX0 = 0;
                    int srcY0 = 0;
                    int copyW = p.w;
                    int copyH = p.h;
                    if (dstX0 < 0) { srcX0 -= dstX0; copyW += dstX0; dstX0 = 0; }
                    if (dstY0 < 0) { srcY0 -= dstY0; copyH += dstY0; dstY0 = 0; }
                    if (dstX0 + copyW > fw) copyW = fw - dstX0;
                    if (dstY0 + copyH > fh) copyH = fh - dstY0;
                    if (copyW <= 0 || copyH <= 0) continue;
                    for (int row = 0; row < copyH; ++row) {
                        const uint32_t* srcRow = p.pixels.data() + (size_t)(srcY0 + row) * p.w + srcX0;
                        uint32_t* dstRow = renderBuffer.data() + (size_t)(dstY0 + row) * fw + dstX0;
                        memcpy(dstRow, srcRow, (size_t)copyW * sizeof(uint32_t));
                    }
                }
            }
            dirty = false;  // Clear after snapshot; new PutImage during render will re-set it

            // Render from staging buffer (no lock held - PutImage can update framebuffer concurrently)
            if (kGpuCompositor && width > 0 && height > 0 && fw > 0 && fh > 0 && !renderBuffer.empty()) {
                /* GPU compositor path: editor layer + each popup are separate
                 * textured quads, composited in z-order on the GPU. Same
                 * letterbox transform as the monolithic path; editor is
                 * top-aligned (y0 = 0) so injectTouch's inverse-scale math is
                 * unchanged. Popups draw on top in stacking order (painter's
                 * algorithm), reproducing the CPU overlay without the memcpy. */
                float scaleX = (float)width / fw;
                float scaleY = (float)height / fh;
                float scale = scaleX < scaleY ? scaleX : scaleY;
                int renderW = (int)(fw * scale);
                int x0 = (width - renderW) / 2;
                int y0 = 0;

                glViewport(0, 0, width, height);
                glClearColor(0.1f, 0.1f, 0.15f, 1.f);
                glClear(GL_COLOR_BUFFER_BIT);
                glUseProgram(program);

                // Editor layer (root-relative rect 0,0,fw,fh).
                uploadTex(fbTex, renderBuffer.data(), fw, fh);
                compositeQuad(fbTex, 0, 0, fw, fh, width, height, x0, y0, scale);

                // Popups on top, in stacking order. One texture per wid; small
                // and visible only briefly, so per-frame re-upload is cheap and
                // robust against missed paint write-sites.
                for (const auto& p : popupSnapshots) {
                    if (p.w <= 0 || p.h <= 0 ||
                        p.pixels.size() != (size_t)p.w * p.h) continue;
                    GLuint& t = popupTextures_[p.wid];
                    if (t == 0) glGenTextures(1, &t);
                    uploadTex(t, p.pixels.data(), p.w, p.h);
                    compositeQuad(t, p.x, p.y, p.w, p.h, width, height, x0, y0, scale);
                }
            } else if (width > 0 && height > 0 && fw > 0 && fh > 0 && !renderBuffer.empty()) {
                // Compute letterbox viewport: scale to fit surface while preserving aspect ratio
                float scaleX = (float)width / fw;
                float scaleY = (float)height / fh;
                float scale = scaleX < scaleY ? scaleX : scaleY;
                int renderW = (int)(fw * scale);
                int renderH = (int)(fh * scale);
                int x0 = (width - renderW) / 2;
                int y0 = (height - renderH) / 2;

                // Clear full surface with background colour first
                glViewport(0, 0, width, height);
                glClearColor(0.1f, 0.1f, 0.15f, 1.f);
                glClear(GL_COLOR_BUFFER_BIT);

                // Render plugin texture top-aligned (not centered).
                // Top-aligning makes touch coordinate math simpler — the
                // editor region starts at surface (0, 0) instead of
                // (x0, y0_centered), so injectTouch's inverse scale lands
                // exactly on the rendered editor.
                y0 = 0;
                glViewport(x0, y0, renderW, renderH);
                glUseProgram(program);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, fbTex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, renderBuffer.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glUniform1i(texUniform, 0);
                glBindBuffer(GL_ARRAY_BUFFER, fbVbo);
                GLint aPos = glGetAttribLocation(program, "aPos");
                GLint aTex = glGetAttribLocation(program, "aTex");
                glEnableVertexAttribArray((GLuint)aPos);
                glVertexAttribPointer((GLuint)aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
                glEnableVertexAttribArray((GLuint)aTex);
                glVertexAttribPointer((GLuint)aTex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
            if (!renderThreadRunning) break;
            if (eglSwapBuffers(eglDisplay, eglSurface) != EGL_TRUE) {
                /* Surface may be destroyed by system; exit immediately without calling
                 * eglMakeCurrent(EGL_NO_SURFACE). Making no context current can trigger
                 * driver-level mutex operations that race with HWUI/audio threads.
                 * Just exit the loop and let the context leak. */
                LOGI("X11Debug: render thread display=%d eglSwapBuffers failed, exiting loop (no eglMakeCurrent)", displayNumber_);
                break;
            }
            thread_local int swapCount = 0;
            if (++swapCount <= 10 || swapCount % 60 == 0) {
                LOGI("X11Debug: render thread display=%d swapped buffer #%d", displayNumber_, swapCount);
            }
        }
        /* Skip EGL teardown: eglDestroyContext/eglTerminate can destroy process-wide
         * driver state and cause "pthread_mutex_lock on destroyed mutex" in HWUI threads
         * that share the same EGL/GL driver. Leak the context to avoid the crash. */
        {
            std::lock_guard<std::mutex> lock(renderExitMutex);
            renderThreadExited.store(true, std::memory_order_release);
        }
        renderExitCv.notify_all();
        LOGI("X11Debug: render thread exiting display=%d tid=%ld (EGL teardown skipped to avoid HWUI mutex crash)", displayNumber_, getTid());
    }

    /* vstpoc 2026-05-24: per-fd writer. Map lookup is brief; sendAll holds
     * only this fd's mutex so a stuck client can't block other fds. */
    std::mutex& writeMutexFor(int fd) {
        std::lock_guard<std::mutex> lk(writeMapMutex);
        auto it = perFdWriteMutex.find(fd);
        if (it == perFdWriteMutex.end())
            it = perFdWriteMutex.emplace(fd, std::make_unique<std::mutex>()).first;
        return *it->second;
    }

    /* Return a STABLE pointer to fd's output queue (creating it). unordered_map
     * guarantees element pointers survive rehash, so once obtained this pointer
     * stays valid even as other fds are added; only the map *structure* needs
     * the lock, which writeMapMutex provides. */
    std::vector<uint8_t>* outQueueFor(int fd) {
        std::lock_guard<std::mutex> lk(writeMapMutex);
        return &outQueue_[fd];
    }

    /* Drain queued bytes (non-blocking). Caller holds writeMutexFor(fd).
     * EAGAIN = "socket full, keep the rest"; false only on fatal socket error. */
    bool drainQueue(int fd, std::vector<uint8_t>* q) {
        if (q->empty()) return true;
        size_t sent = 0;
        while (sent < q->size()) {
            ssize_t n = send(fd, q->data() + sent, q->size() - sent, 0);
            if (n > 0) { sent += (size_t)n; }
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            else return false;
        }
        if (sent) q->erase(q->begin(), q->begin() + sent);
        return true;
    }

    /* Non-blocking, never-drop send. Flush backlog first (preserves order), then
     * try `data`; whatever the socket can't take is appended and flushed later.
     * False only if the client is hopelessly stuck (queue past cap) or socket died. */
    bool sendAllLocked(int fd, const void* data, size_t len) {
        std::vector<uint8_t>* q = outQueueFor(fd);
        std::lock_guard<std::mutex> lock(writeMutexFor(fd));
        if (!drainQueue(fd, q)) return false;
        const uint8_t* p = static_cast<const uint8_t*>(data);
        size_t off = 0;
        if (q->empty()) {
            while (off < len) {
                ssize_t n = send(fd, p + off, len - off, 0);
                if (n > 0) { off += (size_t)n; }
                else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                else return false;
            }
        }
        if (off < len) {
            if (q->size() + (len - off) > kMaxOutQueue) return false;  /* stuck client */
            q->insert(q->end(), p + off, p + len);
        }
        return true;
    }

    /** Send a reply and update lastReplySeq_ for correct event sequence tracking. */
    bool sendReply(const void* data, size_t len, uint16_t replySeq) {
        // Mirror what the Java X server logs at "I xrep ..." so we can
        // diff byte-for-byte: tag is "#<seq>" then a hex dump.
        {
            const uint8_t* b = static_cast<const uint8_t*>(data);
            char hex[3 * 128 + 1];
            size_t n = len < 128 ? len : 128;
            size_t off = 0;
            for (size_t i = 0; i < n; ++i) {
                off += (size_t)snprintf(hex + off, sizeof(hex) - off,
                                        "%02x ", b[i]);
            }
            __android_log_print(ANDROID_LOG_INFO, "xrep",
                "#%u [%zu] %s", (unsigned)replySeq, len, hex);
        }
        bool ok = sendAllLocked(clientFd, data, len);
        if (ok) {
            lastReplySeq_ = replySeq;
            // Mirror into the cross-connection map so sendEvent can look
            // up the right seq when routing events to a different fd.
            std::lock_guard<std::mutex> lk(fdSeqMutex);
            fdLastSeq[clientFd] = replySeq;
        }
        return ok;
    }

    static bool sendAll(int fd, const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        while (len) {
            ssize_t n = send(fd, p, len, 0);
            if (n <= 0) return false;
            p += n;
            len -= n;
        }
        return true;
    }

    /* Receive data with timeout and periodic touch queue draining.
     * This prevents the server thread from blocking for long periods
     * when the plugin is slow to send data. */
    static bool recvAllWithTimeout(int fd, void* data, size_t len, 
                                   std::function<void()> drainTouchCb,
                                   int timeoutMs = 5000) {
        uint8_t* p = static_cast<uint8_t*>(data);
        auto start = std::chrono::steady_clock::now();
        
        while (len > 0) {
            // Check if we've exceeded the timeout
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed > timeoutMs) {
                LOGI("recvAllWithTimeout: timeout after %lld ms, received %zu/%zu bytes", 
                     (long long)elapsed, p - static_cast<uint8_t*>(data), 
                     p - static_cast<uint8_t*>(data) + len);
                return false;
            }
            
            // Poll for data with short timeout
            struct pollfd pfd = { fd, POLLIN, 0 };
            int pollRet = poll(&pfd, 1, 5 /* ms */);
            
            if (pollRet == 0) {
                // No data available, drain touch queue while waiting
                if (drainTouchCb) {
                    drainTouchCb();
                }
                continue;
            }
            
            if (pollRet < 0 || (pfd.revents & (POLLERR | POLLHUP))) {
                return false;
            }
            
            // Data available, receive it
            ssize_t n = recv(fd, p, len, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Would block, try again after draining
                    if (drainTouchCb) {
                        drainTouchCb();
                    }
                    continue;
                }
                return false;
            }
            if (n == 0) {
                return false; // Connection closed
            }
            
            p += n;
            len -= n;
        }
        return true;
    }

    /* recvAll: read exactly len bytes, handling EAGAIN on non-blocking sockets */
    /* Member (not static): flushes the per-fd output queue while waiting for
     * the next request, so buffered events drain instead of stalling until the
     * client sends again — and so the read loop never sits idle on a socket
     * that has pending output. */
    bool recvAll(int fd, void* data, size_t len) {
        uint8_t* p = static_cast<uint8_t*>(data);
        std::vector<uint8_t>* q = outQueueFor(fd);  /* stable pointer */
        int idleMs = 0;  /* accumulated wait with no input — bound at 5s (old behavior) */
        while (len) {
            ssize_t n = recv(fd, p, len, 0);
            if (n > 0) {
                p += n;
                len -= n;
                idleMs = 0;
            } else if (n == 0) {
                return false;  // peer closed
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Non-blocking socket: wait for data. If we have queued
                    // output, also wait for writability + flush it (short
                    // timeout so we re-poll promptly); else just wait for input.
                    bool haveOut;
                    { std::lock_guard<std::mutex> lk(writeMutexFor(fd)); haveOut = !q->empty(); }
                    int waitMs = haveOut ? 50 : 5000;
                    struct pollfd pfd = { fd, (short)(POLLIN | (haveOut ? POLLOUT : 0)), 0 };
                    int ret = poll(&pfd, 1, waitMs);
                    if (ret < 0 || (pfd.revents & (POLLERR | POLLHUP))) return false;
                    if (pfd.revents & POLLOUT) {
                        std::lock_guard<std::mutex> lk(writeMutexFor(fd));
                        if (!drainQueue(fd, q)) return false;
                    }
                    if (!(pfd.revents & POLLIN)) {
                        idleMs += waitMs;
                        if (idleMs >= 5000) return false;  // 5s with no input → give up
                    }
                    continue;
                }
                return false;  // real error
            }
        }
        return true;
    }

    // X11 Connection Setup reply (success). Layout must match libX11/XCB parsing.
    // See X11 Protocol "Connection Setup" and libX11 OpenDis.c.
    //
    // CRITICAL: resource_id_base must be UNIQUE per connection. Wine spawns
    // multiple processes (explorer.exe, wineserver, vst_host.exe), each
    // opens its own X connection. If they all share the same base, their
    // resource ID allocations collide and wine deadlocks waiting for the
    // server to disambiguate. Java X server increments by 0x100000 per
    // connection — we match that here.
    void sendConnectionReply() {
        // Atomic so concurrent accept threads don't both grab 0x100000.
        static std::atomic<uint32_t> sNextResourceIdBase{0x00100000};
        uint32_t myBase = sNextResourceIdBase.fetch_add(0x00100000,
                                                        std::memory_order_relaxed);
        // Installer-mode quirk: when fbSizeFrozen is set, the caller pinned
        // the framebuffer to a known size (e.g. 640x480) and expects wine to
        // place its wizard window inside that. The plain width/height fields
        // hold the SurfaceView pixel dimensions (~1920x1440) — if we report
        // those, wine centers the wizard at SurfaceView center, which is
        // outside the framebuffer and the wizard disappears. Report the
        // framebuffer size instead so wine's default centering lands inside.
        int reportW = width, reportH = height;
        if (fbSizeFrozen && pluginWidth > 0 && pluginHeight > 0) {
            reportW = pluginWidth;
            reportH = pluginHeight;
        }
        auto reply = X11ConnectionHandler::buildConnectionReply(
            byteOrder_, reportW, reportH, myBase);
        LOGI("X11 conn: assigned resource_id_base=0x%08x to fd=%d (screen=%dx%d frozen=%d)",
             (unsigned)myBase, clientFd, reportW, reportH, fbSizeFrozen ? 1 : 0);
        // xrep-tagged hex dump so we can diff against the Java server's
        // setup reply byte-for-byte.
        {
            char hex[3 * 200 + 1];
            size_t n = reply.size() < 200 ? reply.size() : 200;
            size_t off = 0;
            for (size_t i = 0; i < n; ++i)
                off += (size_t)snprintf(hex + off, sizeof(hex) - off,
                                        "%02x ", reply[i]);
            __android_log_print(ANDROID_LOG_INFO, "xrep",
                "#0 setup [%zu] %s", reply.size(), hex);
        }
        if (!sendAllLocked(clientFd, reply.data(), reply.size())) {
            LOGE("X11 connection reply: send failed");
        }
    }

    void sendError(uint8_t code, uint16_t seq, uint32_t badValue) {
        uint8_t buf[32];
        memset(buf, 0, 32);
        buf[0] = 0;  // Error
        buf[1] = code;
        write16(buf, 2, seq);
        write32(buf, 4, badValue);
        sendAllLocked(clientFd, buf, 32);
    }

    // X11 GetGeometry reply: reply(1), depth(1), seq(2), length(4),
    //   root(4), x(2), y(2), width(2), height(2), border_width(2), pad(2).
    void sendReplyGetGeometry(uint16_t seq, uint32_t root, int x, int y, int w, int h) {
        uint8_t buf[32];
        memset(buf, 0, 32);
        buf[0] = 1;  // Reply
        buf[1] = 24; // depth (24-bit TrueColor) — byte 1 per X11 protocol
        write16(buf, 2, seq);
        write32(buf, 4, 0);   // length
        write32(buf, 8, root);
        write16(buf, 12, (uint16_t)(int16_t)x);
        write16(buf, 14, (uint16_t)(int16_t)y);
        write16(buf, 16, (uint16_t)w);
        write16(buf, 18, (uint16_t)h);
        write16(buf, 20, 0);  // border_width
        sendReply(buf, 32, seq);
    }

    /** Send Expose events to root + all child windows to force a full redraw.
     *  Called after resuming from hidden state so the plugin repaints everything. */
    void sendExposeToAllWindows() {
        if (clientFd < 0) return;
        uint16_t evtSeq = lastReplySeq_;

        auto sendExpose = [&](uint32_t wid) {
            int w = width, h = height;
            auto sz = windowManager_.getSize(wid);
            if (sz.first > 0 && sz.second > 0) {
                w = sz.first;
                h = sz.second;
            }
            uint8_t evt[32];
            memset(evt, 0, 32);
            evt[0] = Expose;
            write16(evt, 2, evtSeq);
            write32(evt, 4, wid);
            write16(evt, 8, 0);
            write16(evt, 10, 0);
            write16(evt, 12, (uint16_t)w);
            write16(evt, 14, (uint16_t)h);
            write16(evt, 16, 0);  // count=0 (no more Expose events follow)
            sendAllLocked(clientFd, evt, 32);
            LOGI("X11 resume: sent Expose for window 0x%x %dx%d", wid, w, h);
        };

        // Expose root window
        sendExpose(kRootWindowId);
        // Expose all child windows
        for (uint32_t wid : childWindows) {
            sendExpose(wid);
        }
        LOGI("X11 resume: sent Expose to %zu windows (root + children)", 1 + childWindows.size());
    }

    /* Drain queued touch events and send them on the socket.
     * MUST be called only from the server thread, between request processing.
     *
     * Experiment: buffer drag (action==2) events and only flush at 30fps,
     * followed by an Expose to trigger a full _expose() redraw. */
    bool hasPendingDrag = false;
    int pendingDragX = 0, pendingDragY = 0;
    uint32_t grabWindow = 0;  // Window that captured the pointer on ButtonPress

    /* Active X11 pointer grab (XGrabPointer / case 26), distinct from
     * grabWindow (our per-drag button grab). Wine's winex11.drv grabs with
     * owner_events=FALSE for menu mode (X11DRV_SetCapture, GUI_INMENUMODE):
     *   XGrabPointer(menu_whole_window, False, Button|Motion, ...)
     * Per X semantics, owner_events=False means EVERY pointer event goes to
     * the grab window regardless of geometry. JUCE menus rely on this: they
     * receive the outside-click (at out-of-bounds local coords, real root
     * coords) and dismiss; wine then XUngrabPointers. Our old stub acked the
     * grab but routed by geometry, so outside-clicks hit the main editor and
     * the menu could never close ("UI freeze"). Set/cleared on the connection
     * thread (case 26/27), read on the touch-drain thread → atomic.
     * See feedback_x50_stomp_popup_grab_dismiss. */
    std::atomic<uint32_t> xPointerGrabWindow_{0};
    std::atomic<bool>     xPointerGrabOwnerEvents_{false};

    std::chrono::steady_clock::time_point lastExposeFlush = std::chrono::steady_clock::now();
    static constexpr double FLUSH_INTERVAL_SEC = kTouchFlushIntervalSec;

    // Send event to a specific child window with local coordinates
    void sendEventToChild(uint8_t type, int globalX, int globalY, int button, uint16_t seq, int stateOverride = -1) {
        HitResult hit;
        if (grabWindow != 0) {
            // During drag, keep sending to the grabbed window.
            // For smart-flipped popups, the windowManager position is wine's
            // requested (pre-flip) coord — using it here gives a NEGATIVE
            // local Y when the user's finger is above wine's view of the
            // popup. sendEvent then computes root_y = reqY + (negative) =
            // framebuffer y, and wine's cursor jumps OUTSIDE the popup rect
            // → WM_MOUSELEAVE → JUCE dismisses the menu on the very next
            // drag/release event. Use the DISPLAYED position from
            // popupOverlays so local stays in [0, h).
            int absX = 0, absY = 0;
            {
                std::lock_guard<std::mutex> fbLock(bufferMutex);
                auto pit = popupOverlays.find(grabWindow);
                if (pit != popupOverlays.end()) {
                    absX = pit->second.x;
                    absY = pit->second.y;
                } else {
                    auto absPos = getAbsolutePos(grabWindow);
                    absX = absPos.first;
                    absY = absPos.second;
                }
            }
            hit = {grabWindow, globalX - absX, globalY - absY};
        } else {
            hit = hitTestChildWindow(globalX, globalY);
        }
        sendEvent(type, hit.wid, hit.localX, hit.localY, button, seq, stateOverride);
    }

    void drainTouchQueue() {
        /* Only the connection thread that owns the editor window should
         * deliver touch events. Otherwise multiple wine processes would each
         * forward the same event (X11 event delivery is per-connection) and
         * the plugin gets duplicate / spurious clicks. */
        int owner = editorOwnerFd.load(std::memory_order_acquire);
        if (owner < 0 || owner != clientFd) {
            return;
        }
        std::vector<QueuedTouch> pending;
        {
            std::lock_guard<std::mutex> lock(touchQueueMutex);
            pending.swap(touchQueue);
        }

        uint16_t seq = lastReplySeq_;

        // Route the touch to the editor window of the slot the user
        // touched (set by injectTouch from the surface-y position).
        // Falls back to hit-test result when no slot is registered (the
        // single-plugin legacy path).
        int slot = lastTouchSlot.load(std::memory_order_acquire);
        uint32_t slotWid = 0;
        if (slot >= 0 && slot < (int)pluginSlotWindows.size()) {
            slotWid = pluginSlotWindows[slot];
        }
        if (!pending.empty()) {
            LOGI("X11 drainTouchQueue display=%d slot=%d slotWid=0x%x slotsAvail=%zu pendingTouches=%zu",
                 displayNumber_, slot, slotWid, pluginSlotWindows.size(), pending.size());
        }

        // Process button press/release immediately; buffer drag moves
        for (auto& t : pending) {
            lastPointerX.store(t.x, std::memory_order_relaxed);
            lastPointerY.store(t.y, std::memory_order_relaxed);
            if (t.action == 0) {
                // DEBUG: re-read the focus-emulation A/B mask on every touch-down
                // so variants can be toggled live (no relaunch). See focusModeMask_.
                {
                    uint32_t fm = readFocusMode();
                    uint32_t prevFm = focusModeMask_.exchange(fm, std::memory_order_relaxed);
                    if (fm != prevFm) LOGI("X11 focusMode -> 0x%x (was 0x%x)", fm, prevFm);
                }
                // Flush any pending drag before ButtonPress
                if (hasPendingDrag) {
                    sendEventToChild(MotionNotify, pendingDragX, pendingDragY, 0, seq);
                    hasPendingDrag = false;
                }
                // Hit-test and grab the child window. Prefer the
                // hit-test result — JUCE-style plugins draw and listen
                // on a deep child window, and routing to the top-level
                // (slotWid) drops events on the floor. Use slotWid only
                // as a fallback when hit-test finds nothing better than
                // the root.
                HitResult hit;
                bool fellBack = false;
                /* Honor wine's owner_events=False pointer grab — but ONLY when
                 * the grab window is a real visible popup we composite (a JUCE
                 * menu / combo dropdown). Wine grabs owner_events=False in TWO
                 * cases: menu mode (grab = the menu's whole_window, which IS a
                 * renderable popup) and cursor clipping for infinite knob-drag
                 * (grab = wine's internal clip_window, NOT a popup). Scoping to
                 * renderable-popup grabs steals events for menu dismissal while
                 * leaving knob-drag cursor-clip grabs on the normal geometry
                 * path. */
                bool xgrabSteals = false;
                int  xgrabAbsX = 0, xgrabAbsY = 0;
                uint32_t xgrab = xPointerGrabWindow_.load(std::memory_order_acquire);
                if (xgrab != 0 && !xPointerGrabOwnerEvents_.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> fbLock(bufferMutex);
                    auto pit = popupOverlays.find(xgrab);
                    if (pit != popupOverlays.end() && isRenderablePopup(pit->second)) {
                        xgrabSteals = true;
                        xgrabAbsX = pit->second.x;
                        xgrabAbsY = pit->second.y;
                    }
                }
                if (xgrabSteals) {
                    /* Deliver the press to the menu even when the touch is
                     * OUTSIDE its bounds, so JUCE gets the outside-click it
                     * needs to dismiss. Local coords are relative to the grab
                     * window (may be negative / out of bounds); sendEvent fills
                     * the real root coords from t.x/t.y, which is what JUCE uses
                     * to hit-test across its menu/submenu windows. Bypasses the
                     * slot fallback + dead-overlay redirect — under the menu
                     * grab the menu wins unconditionally. */
                    hit.wid = xgrab;
                    hit.localX = t.x - xgrabAbsX;
                    hit.localY = t.y - xgrabAbsY;
                } else {
                hit = hitTestChildWindow(t.x, t.y);
                if (slotWid && (hit.wid == 0 || hit.wid == kRootWindowId)) {
                    hit.wid = slotWid;
                    hit.localX = t.x;
                    hit.localY = t.y;
                    fellBack = true;
                }
                /* Dead-overlay redirect (BIAS FX 2's CEF editor): the tap can
                 * hit-test to a maskless overlay window (BIAS stacks a huge
                 * 3072x1620 window with event-mask=0 over the content) that is a
                 * SIBLING of the real editor under the desktop — so neither the
                 * click nor the focus-walk reaches the editor and wine focuses
                 * the desktop instead, dropping all keyboard input. If the hit
                 * window selects no ButtonPress, redirect to the real editor:
                 * the mapped NON-desktop window (parent != root) that DOES carry
                 * ButtonPressMask and contains the tap point (where rendering
                 * goes). No-op for normal plugins whose editor selects input. */
                if (!fellBack) {
                    std::lock_guard<std::mutex> mapLock(windowMapMutex);
                    if (!(windowManager_.getEventMask(hit.wid) & 0x4 /*ButtonPressMask*/)) {
                        uint32_t best = 0; long long bestArea = -1;
                        for (uint32_t cw : windowManager_.childWindows()) {
                            if (windowManager_.isUnmapped(cw)) continue;
                            auto p = windowManager_.getPosition(cw);
                            if (!(windowManager_.getEventMask(cw) & 0x4)) continue;
                            /* Skip the wine DESKTOP (parent=root, no input mask) but
                             * NOT a legit top-level content window that carries
                             * ButtonPress — BIAS FX 2's CEF browser window 0x700001 is
                             * top-level (parent=root) with mask 0x62c05f and IS the real
                             * input target sitting under a maskless GPU-compositor
                             * overlay. The ButtonPress check above already excludes the
                             * desktop (SubstructureRedirect only), so only skip a
                             * parent=root window if it ALSO lacks ButtonPress. */
                            if ((p.parent == 0 || p.parent == kRootWindowId) &&
                                !(windowManager_.getEventMask(cw) & 0x4)) continue;
                            auto ap = windowManager_.getAbsolutePos(cw);
                            auto sz = windowManager_.getSize(cw);
                            if (t.x < ap.first || t.x >= ap.first + sz.first ||
                                t.y < ap.second || t.y >= ap.second + sz.second) continue;
                            long long area = (long long)sz.first * (long long)sz.second;
                            if (bestArea < 0 || area < bestArea) { best = cw; bestArea = area; }
                        }
                        if (best) {
                            auto ap = windowManager_.getAbsolutePos(best);
                            hit.wid = best;
                            hit.localX = t.x - ap.first;
                            hit.localY = t.y - ap.second;
                        }
                    }
                }
                }  /* end !xgrabSteals */
                grabWindow = hit.wid;
                uint32_t hitMask = 0;
                {
                    std::lock_guard<std::mutex> mapLock(windowMapMutex);
                    hitMask = windowManager_.getEventMask(hit.wid);
                }
                LOGI("X11 touch DOWN route display=%d fb=(%d,%d) hit=0x%x local=(%d,%d) slotWid=0x%x fallback=%d mask=0x%x",
                     displayNumber_, t.x, t.y, hit.wid, hit.localX, hit.localY,
                     slotWid, fellBack ? 1 : 0, hitMask);
                // Click-to-focus. Real X11 setups rely on a window manager
                // to call XSetInputFocus when the user clicks a window, which
                // then emits FocusIn. We have no WM, so wine sees clicks but
                // never sees its top-level become focused — and without that,
                // EDIT controls never receive WM_SETFOCUS, the caret never
                // appears, and KeyPress events go nowhere when delivered.
                //
                // The wine HWND-backing window is the topmost ancestor in our
                // tracked tree that has KeyPressMask in its event mask. The
                // wine "desktop" window above it is reparented to root but
                // only carries SubstructureRedirectMask — focusing the desktop
                // is useless because it doesn't subscribe to keyboard input.
                {
                    uint32_t topLevel = hit.wid;
                    uint32_t bestKeyTarget = 0;  // highest NON-DESKTOP ancestor w/ KeyPressMask
                    uint32_t vdeskWindow = 0;    // wine virtual-desktop window (top-level child of our real root)
                    for (int depth = 0; depth < 32; ++depth) {
                        std::lock_guard<std::mutex> mapLock(windowMapMutex);
                        auto pos = windowManager_.getPosition(topLevel);
                        bool isDesktop = (pos.parent == 0 || pos.parent == kRootWindowId);
                        uint32_t mask = windowManager_.getEventMask(topLevel);
                        /* Don't target the wine desktop window (the top-level
                         * directly under our real root). Both the editor AND the
                         * desktop carry KeyPressMask; picking the HIGHEST lands
                         * on the desktop, so wine focuses the desktop instead of
                         * the editor and keys never reach the plugin — BIAS
                         * FX 2's CEF editor gets its keyboard focus stuck on the
                         * desktop (focus=desktop while fg=editor). Stop at the
                         * editor: the highest KeyPressMask window that is NOT the
                         * desktop. */
                        if ((mask & 0x1 /*KeyPressMask*/) && !isDesktop) bestKeyTarget = topLevel;
                        if (isDesktop) { vdeskWindow = topLevel; break; }
                        topLevel = pos.parent;
                    }
                    /* TODO(BIAS CEF text input — resume here next session): root
                     * cause PROVEN (Xvfb A/B) — in virtual-desktop mode wine
                     * activates its foreground (→ CEF WebContents → caret/typing)
                     * only when its activatable explorer.exe FRAME top-level gets
                     * FocusIn; wine ignores FocusIn to the #32769 desktop
                     * (X11DRV_FocusIn event.c:927). Tried: (1) focus the desktop
                     * 0x200007 → ignored; (2) `explorer.exe /desktop=` launch →
                     * frame created but as a separate top-level NOT in the editor's
                     * parent chain + a stray console; (3) focus the largest
                     * non-desktop root-child = approach A → did not land. See
                     * memory feedback_bias_text_input_lag.md cont.8–11 for the full
                     * map + remaining ideas (re-parent editor under frame; suppress
                     * console via FreeConsole/DETACHED_PROCESS; identify the frame
                     * by WM_CLASS=explorer.exe). For now: safe editor focus. */
                    (void)vdeskWindow;
                    if (bestKeyTarget) topLevel = bestKeyTarget;
                    uint32_t fm = focusModeMask_.load(std::memory_order_relaxed);
                    /* Suppress click-to-focus when the walk cannot find a
                     * non-desktop key target. That case is usually a
                     * desktop-parented JUCE popup/dialog: sending synthetic
                     * FocusIn + WM_TAKE_FOCUS there re-enters wine's activation
                     * path before the ButtonPress and can wedge every guest
                     * thread in pipe_read (X50II popup freeze). The main editor
                     * host-frame path still finds a child key target and keeps
                     * normal focus behavior. 0x20 remains the hard kill switch
                     * for all click-to-focus. */
                    bool fmSkipAll = (fm & 0x20u) || (bestKeyTarget == 0);
                    uint32_t prev = fmSkipAll ? topLevel
                                  : focusedWindowId.exchange(topLevel, std::memory_order_acq_rel);
                    if (!fmSkipAll && prev != topLevel) {
                        auto sendFocus = [&](uint8_t type, uint32_t w) {
                            if (!w) return;
                            int fd = fdForWindow(w);
                            uint16_t evtSeq = seq;
                            if (fd != clientFd) {
                                std::lock_guard<std::mutex> lk(fdSeqMutex);
                                auto it = fdLastSeq.find(fd);
                                if (it != fdLastSeq.end()) evtSeq = it->second;
                            }
                            uint8_t evt[32];
                            memset(evt, 0, 32);
                            evt[0] = type;
                            evt[1] = 3;  /* detail = NotifyNonlinear */
                            write16(evt, 2, evtSeq);
                            write32(evt, 4, w);
                            evt[8] = 0;  /* mode = NotifyNormal */
                            sendAllLocked(fd, evt, 32);
                        };
                        if (!(fm & 0x2u)) {   /* 0x2 = suppress synthetic FocusOut/FocusIn */
                            if (prev) sendFocus(10 /*FocusOut*/, prev);
                            sendFocus(9 /*FocusIn*/, topLevel);
                        }
                        /* Wine's X11DRV_FocusIn early-returns when
                         * use_take_focus=TRUE (the default), expecting a
                         * window manager to send a WM_TAKE_FOCUS
                         * ClientMessage that triggers set_focus →
                         * NtUserSetForegroundWindow. We run without a WM,
                         * so without this synthetic WM_TAKE_FOCUS wine
                         * has no foreground after the FocusIn — INPUT_
                         * KEYBOARD posts from X11DRV_KeyEvent get dropped
                         * (no thread to deliver to) and EDIT controls in
                         * plugin dialogs never see typed characters.
                         * Wine handles WM_TAKE_FOCUS unconditionally:
                         * see X11DRV_ClientMessage handler in event.c. */
                        if (topLevel && !(fm & 0x1u)) {   /* 0x1 = suppress synthetic WM_TAKE_FOCUS */
                            uint32_t protocolsAtom = atoms_.intern("WM_PROTOCOLS", false);
                            uint32_t takeFocusAtom = atoms_.intern("WM_TAKE_FOCUS", false);
                            int fd = fdForWindow(topLevel);
                            uint16_t evtSeq = seq;
                            if (fd != clientFd) {
                                std::lock_guard<std::mutex> lk(fdSeqMutex);
                                auto it = fdLastSeq.find(fd);
                                if (it != fdLastSeq.end()) evtSeq = it->second;
                            }
                            uint8_t cm[32];
                            memset(cm, 0, 32);
                            cm[0] = 33;          /* ClientMessage */
                            cm[1] = 32;          /* format = 32-bit data */
                            write16(cm, 2, evtSeq);
                            write32(cm, 4, topLevel);
                            write32(cm, 8, protocolsAtom);
                            write32(cm, 12, takeFocusAtom);  /* data[0] */
                            write32(cm, 16, x11Timestamp()); /* data[1] = time */
                            sendAllLocked(fd, cm, 32);
                        }
                        static thread_local int dbgF = 0;
                        if (++dbgF <= 20) {
                            LOGI("X11 click-to-focus: top=0x%x prev=0x%x (hit=0x%x) fm=0x%x",
                                 topLevel, prev, hit.wid, fm);
                        }
                    }
                }
                // ButtonPress must precede the synthetic MotionNotify. xputty's main
                // loop coalesces consecutive MotionNotify events via
                // XCheckTypedWindowEvent, which scans past an intervening ButtonPress;
                // if the synthetic comes first, the next drag MotionNotify gets merged
                // with it and dispatched before ButtonPress — knob widgets then run
                // adj_set_motion_state with uninitialized pos_y/start_value and snap
                // the value to minimum.
                /* 0x40 = send a hover MotionNotify (and 0x80 an EnterNotify)
                 * BEFORE the press. Real X always precedes a click with pointer
                 * motion INTO the target window; our touch path jumps straight to
                 * ButtonPress. Chromium's render widget (BIAS CEF) may require that
                 * prior WM_MOUSEMOVE / mouse-enter to treat the click as an in-widget
                 * click that focuses the DOM element (no caret without it). Gated
                 * because motion-before-press otherwise breaks xputty knob widgets. */
                {
                    uint32_t fm = focusModeMask_.load(std::memory_order_relaxed);
                    if (fm & 0x80u) {
                        uint8_t en[32];
                        memset(en, 0, 32);
                        en[0] = 7;                 /* EnterNotify */
                        en[1] = 0;                 /* detail = NotifyAncestor */
                        write16(en, 2, seq);
                        write32(en, 4, x11Timestamp());  /* time */
                        write32(en, 8, kRootWindowId);   /* root */
                        write32(en, 12, hit.wid);        /* event window */
                        write32(en, 16, 0);              /* child = None */
                        write16(en, 20, (uint16_t)hit.localX); /* root-x (approx) */
                        write16(en, 22, (uint16_t)hit.localY); /* root-y */
                        write16(en, 24, (uint16_t)hit.localX); /* event-x */
                        write16(en, 26, (uint16_t)hit.localY); /* event-y */
                        write16(en, 28, 0);        /* state */
                        en[30] = 0;                /* mode = NotifyNormal */
                        en[31] = 1;                /* same-screen, focus */
                        int fd = fdForWindow(hit.wid);
                        sendAllLocked(fd, en, 32);
                    }
                    if (fm & 0x40u)
                        sendEvent(MotionNotify, hit.wid, hit.localX, hit.localY, 0, seq, 0);
                }
                pointerButton1Down.store(true, std::memory_order_relaxed);
                sendEvent(ButtonPress, hit.wid, hit.localX, hit.localY, 1, seq);
                // Synthetic MotionNotify (state=0, no buttons) so combobox popups set
                // prelight_item from the touch position (on desktop, mouse hover does
                // this). state=0 skips xputty's adj_set_motion_state drag math but
                // still fires motion_callback.
                sendEvent(MotionNotify, hit.wid, hit.localX, hit.localY, 0, seq, 0);
            } else if (t.action == 1) {
                // Flush any pending drag before ButtonRelease
                if (hasPendingDrag) {
                    sendEventToChild(MotionNotify, pendingDragX, pendingDragY, 0, seq);
                    hasPendingDrag = false;
                }
                pointerButton1Down.store(false, std::memory_order_relaxed);
                LOGI("X11 touch UP route display=%d fb=(%d,%d) grab=0x%x",
                     displayNumber_, t.x, t.y, grabWindow);
                sendEventToChild(ButtonRelease, t.x, t.y, 1, seq);
                grabWindow = 0;  // Release grab
            } else if (t.action == 3) {
                /* Right-click tap: atomic ButtonPress(3) + ButtonRelease(3) at
                 * the same point. Triggered from the Android UI by a two-finger
                 * tap (PluginSurface.kt). No drag/grab state is set — wine's
                 * x11drv translates X11 button 3 into WM_RBUTTONDOWN/UP and the
                 * plugin shows its context menu. We deliberately skip the
                 * click-to-focus dance used for button 1 because right-clicks
                 * shouldn't move keyboard focus. */
                if (hasPendingDrag) {
                    sendEventToChild(MotionNotify, pendingDragX, pendingDragY, 0, seq);
                    hasPendingDrag = false;
                }
                HitResult hit = hitTestChildWindow(t.x, t.y);
                if (slotWid && (hit.wid == 0 || hit.wid == kRootWindowId)) {
                    hit.wid = slotWid;
                    hit.localX = t.x;
                    hit.localY = t.y;
                }
                pointerButton3Down.store(true, std::memory_order_relaxed);
                sendEvent(ButtonPress, hit.wid, hit.localX, hit.localY, 3, seq);
                pointerButton3Down.store(false, std::memory_order_relaxed);
                sendEvent(ButtonRelease, hit.wid, hit.localX, hit.localY, 3, seq);
            } else {
                // Drag move: send MotionNotify immediately to the grabbed window so
                // drag-based controls (faders/knobs) track the pointer. The prior
                // 30fps-buffered flush could drop motion entirely for some plugins
                // (DeeGain's fader received 0 MotionNotify across 174 moves). injectTouch
                // already coalesces consecutive moves, so this is ~1 event per drain.
                static thread_local int dragDbg = 0;
                if (++dragDbg <= 20)
                    LOGI("X11 DRAG move (%d,%d) grab=0x%x -> MotionNotify", t.x, t.y, grabWindow);
                sendEventToChild(MotionNotify, t.x, t.y, 0, seq);
            }
        }

        // Flush buffered drag at 30fps + send Expose for full redraw
        if (hasPendingDrag && clientFd >= 0) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - lastExposeFlush).count();
            if (elapsed >= FLUSH_INTERVAL_SEC) {
                sendEventToChild(MotionNotify, pendingDragX, pendingDragY, 0, seq);
                hasPendingDrag = false;
                lastExposeFlush = now;
            }
        }

        // Drain key events. Priority order:
        //   1. focusedWindowId — wine called SetInputFocus on this wid, meaning
        //      it's the HWND's X11 backing window that should receive keys.
        //   2. slotWid / pluginSlotWindows[0] — top-level editor window.
        //      Important: wine sets KeyPressMask only on top-level windows.
        //      Child X11 windows backing inner widgets (EDIT controls etc.)
        //      have no event mask, so sending KeyPress to them is silently
        //      dropped by wine's libX11 dispatcher. Keys MUST go to a window
        //      with KeyPressMask; wine then routes WM_KEYDOWN internally to
        //      the focused child HWND.
        //   3. grabWindow — last fallback if no top-level exists.
        std::vector<QueuedKey> keysPending;
        {
            std::lock_guard<std::mutex> lock(keyQueueMutex);
            keysPending.swap(keyQueue);
        }
        if (!keysPending.empty()) {
            uint32_t focused = focusedWindowId.load(std::memory_order_acquire);
            uint32_t target = focused;
            /* Never drain keys to the wine DESKTOP window or to a window that
             * doesn't select KeyPressMask: wine sometimes sets SetInputFocus to
             * the desktop (BIAS FX 2 — focus lands on the desktop, not the CEF
             * editor), so the keystrokes would vanish into the void. When the
             * focus target is the desktop / maskless / unset, route keys to the
             * real editor = the mapped NON-desktop window that carries
             * KeyPressMask (same window the click-redirect targets). */
            {
                std::lock_guard<std::mutex> lk(windowMapMutex);
                bool ok = false;
                if (target) {
                    auto p = windowManager_.getPosition(target);
                    bool isDesktop = (p.parent == 0 || p.parent == kRootWindowId);
                    ok = !isDesktop && (windowManager_.getEventMask(target) & 0x1 /*KeyPressMask*/);
                }
                if (!ok) {
                    uint32_t best = 0; long long bestArea = -1;
                    for (uint32_t cw : windowManager_.childWindows()) {
                        if (windowManager_.isUnmapped(cw)) continue;
                        auto p = windowManager_.getPosition(cw);
                        if (p.parent == 0 || p.parent == kRootWindowId) continue;
                        if (!(windowManager_.getEventMask(cw) & 0x1 /*KeyPressMask*/)) continue;
                        auto sz = windowManager_.getSize(cw);
                        long long area = (long long)sz.first * (long long)sz.second;
                        if (bestArea < 0 || area < bestArea) { best = cw; bestArea = area; }
                    }
                    if (best) target = best;
                }
            }
            if (target == 0) {
                target = slotWid;
                if (target == 0 && !pluginSlotWindows.empty()) target = pluginSlotWindows[0];
                if (target == 0) target = grabWindow;
            }
            const int px = lastPointerX.load(std::memory_order_relaxed);
            const int py = lastPointerY.load(std::memory_order_relaxed);
            constexpr uint16_t kShiftMask = 1 << 0;
            constexpr uint16_t kCtrlMask  = 1 << 2;
            constexpr uint8_t  kShiftLKc  = 65;  // XK_Shift_L in our kJavaKeymapPayload
            constexpr uint8_t  kCtrlLKc   = 67;  // XK_Control_L in our kJavaKeymapPayload
            for (auto& k : keysPending) {
                const uint8_t type = (k.action == 0) ? KeyPress : KeyRelease;
                /* Wine's win32u/winex11.drv tracks modifier state per-thread
                 * by listening for SHIFT/CTRL key press/release events — it
                 * does NOT derive the modifier state from the `state` field
                 * of subsequent KeyPress events. Result: typing "A" via just
                 * `KeyPress(keycode=24, state=Shift)` lands as lowercase "a"
                 * because wine thinks no modifier is down. Discovered
                 * 2026-05-27 by diffing the on-the-wire POST body Helix
                 * Native sends: the user typed "Dy70fE4BIEmd" but Line 6
                 * received "dy70fe4biemd".
                 *
                 * Fix: wrap each shifted/ctrl'd key with a synthetic press
                 * of the modifier on KeyPress, release on KeyRelease. Wine
                 * then sees the modifier go down → updates its tracked
                 * state → ToUnicode returns the correct shifted char. */
                if (type == KeyPress) {
                    if (k.state & kShiftMask)
                        sendEvent(KeyPress, target, px, py, kShiftLKc, seq, 0);
                    if (k.state & kCtrlMask)
                        sendEvent(KeyPress, target, px, py, kCtrlLKc, seq,
                                  k.state & kShiftMask);
                }
                sendEvent(type, target, px, py, k.keycode, seq, k.state);
                if (type == KeyRelease) {
                    if (k.state & kCtrlMask)
                        sendEvent(KeyRelease, target, px, py, kCtrlLKc, seq,
                                  k.state & kShiftMask);
                    if (k.state & kShiftMask)
                        sendEvent(KeyRelease, target, px, py, kShiftLKc, seq, 0);
                }
            }
            static thread_local int dbgK = 0;
            if (++dbgK <= 20) {
                LOGI("X11 drainKeyQueue display=%d sent %zu key events to wid=0x%x",
                     displayNumber_, keysPending.size(), target);
            }
        }
    }

    // Hit-test: try popup overlays first at their DISPLAYED (post-flip)
    // position so taps on a flipped popup route to its wid even though
    // windowManager_ still has wine's unflipped coords. Falls through to
    // windowManager_.hitTest() for non-popup windows.
    // lockMap controls whether windowMapMutex is acquired (needed from UI thread).
    HitResult hitTestChildWindow(int x, int y, bool lockMap = false) {
        std::unique_lock<std::mutex> mapLock(windowMapMutex, std::defer_lock);
        if (lockMap) mapLock.lock();
        /* Popup overlay sweep — iterate in REVERSE stacking order
         * (topmost first) so nested popups are preferred over their parents.
         * Use isRenderablePopup() so hit-testing matches what the compositor
         * actually draws: only real menu/combo/dialog bodies are hittable, not
         * the thin shadow strips (a click on an invisible strip is wrong) nor
         * wine's unpainted internal helper windows. */
        {
            std::lock_guard<std::mutex> fbLock(bufferMutex);
            const auto& cws = windowManager_.childWindows();
            for (auto it = cws.rbegin(); it != cws.rend(); ++it) {
                auto pit = popupOverlays.find(*it);
                if (pit == popupOverlays.end()) continue;
                const auto& p = pit->second;
                if (!isRenderablePopup(p)) continue;
                if (x >= p.x && x < p.x + p.w && y >= p.y && y < p.y + p.h) {
                    HitResult r;
                    r.wid = *it;
                    r.localX = x - p.x;
                    r.localY = y - p.y;
                    static std::atomic<uint32_t> popupHitLog{0};
                    uint32_t n = ++popupHitLog;
                    if (n <= 120 || n % 50 == 0) {
                        LOGI("X11 popup HIT #%u wid=0x%x fb=(%d,%d) display=(%d,%d) req=(%d,%d) size=%dx%d local=(%d,%d) content=%d",
                             n, *it, x, y, p.x, p.y, p.reqX, p.reqY, p.w, p.h,
                             r.localX, r.localY, p.hasContent ? 1 : 0);
                    }
                    return r;
                }
            }
        }
        return windowManager_.hitTest(x, y);
    }

    uint32_t x11Timestamp() {
        using namespace std::chrono;
        return (uint32_t)duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
    }

    /** Find the fd of the X11 connection that created `wid`. Falls back to
     *  the connection-thread's own clientFd if not tracked. Used when
     *  routing synthetic input events to a window owned by a different
     *  wine client than the one currently running drainTouchQueue — wine
     *  expects events on the same connection it created the window on. */
    int fdForWindow(uint32_t wid) {
        std::lock_guard<std::mutex> lock(windowCreatorMutex);
        auto it = windowCreator.find(wid);
        if (it != windowCreator.end()) return it->second;
        return clientFd;
    }

    void sendEvent(uint8_t type, uint32_t windowId, int x, int y, int button, uint16_t lastSeq, int stateOverride = -1) {
        uint8_t buf[32];
        memset(buf, 0, 32);
        buf[0] = type;
        // X11 event "detail" field. For button events it's the button #;
        // for key events it's the X11 keycode. The button param doubles as
        // the keycode carrier when type == KeyPress/KeyRelease.
        buf[1] = (type == ButtonPress || type == ButtonRelease ||
                  type == KeyPress    || type == KeyRelease)
                 ? (uint8_t)button : 0;
        // Look up the right seq for the TARGET connection. The caller's
        // lastSeq is from the current thread's connection — if we end up
        // routing to a different fd (different wine client owns the
        // window), libX11 on that client must see a seq that matches
        // ITS own request counter, not ours, or the event gets queued as
        // "from a future request" and may never dispatch to WindowProc.
        int targetFd = fdForWindow(windowId);
        uint16_t outSeq = lastSeq;
        if (targetFd != clientFd) {
            std::lock_guard<std::mutex> lk(fdSeqMutex);
            auto it = fdLastSeq.find(targetFd);
            if (it != fdLastSeq.end()) outSeq = it->second;
        }
        // x, y are LOCAL coordinates within windowId. Compute root_x/root_y
        // in WINE's coordinate space — for popups that we smart-flipped, wine
        // believes the popup is still at its requested (reqX, reqY) position,
        // and the menu-loop's "is this click inside the popup?" test compares
        // root coords against that requested rectangle. Sending displayed-pos
        // root coords made wine see clicks as outside → popup dismissed.
        int rootX = x, rootY = y;
        {
            std::lock_guard<std::mutex> lk(windowMapMutex);
            auto pit = popupOverlays.find(windowId);
            if (pit != popupOverlays.end()) {
                rootX = pit->second.reqX + x;
                rootY = pit->second.reqY + y;
            } else {
                auto abs = windowManager_.getAbsolutePos(windowId);
                rootX = abs.first + x;
                rootY = abs.second + y;
            }
        }
        write16(buf, 2, outSeq);                // sequence number
        write32(buf, 4, x11Timestamp());        // time
        write32(buf, 8, kRootWindowId);        // root window
        write32(buf, 12, windowId);             // event window
        write32(buf, 16, 0);                    // child window (None)
        write16(buf, 20, (uint16_t)(int16_t)rootX); // root-x
        write16(buf, 22, (uint16_t)(int16_t)rootY); // root-y
        write16(buf, 24, (uint16_t)(int16_t)x);     // event-x
        write16(buf, 26, (uint16_t)(int16_t)y);     // event-y
        uint16_t state = 0;
        if (stateOverride >= 0) {
            state = (uint16_t)stateOverride;
        } else {
            if (type == ButtonRelease && button == 1) state = (1 << 8); // Button1Mask
            if (type == MotionNotify) state = (1 << 8); // Button1Mask while dragging
        }
        write16(buf, 28, state);                // state
        buf[30] = 1;                            // same-screen = True
        // Route to the connection that created this window (targetFd
        // already computed above). Wine expects events on the same
        // connection it created the window on; sending through a
        // different client's fd lands the event on the wrong user32
        // dispatcher and the plugin's WindowProc never sees it.
        bool ok = sendAllLocked(targetFd, buf, 32);
        if (type == ButtonPress || type == ButtonRelease) {
            static std::atomic<uint32_t> vstpocSent{0};
            uint32_t n = ++vstpocSent;
            LOGI("X11 vstpoc-send: #%u type=%s wid=0x%x targetFd=%d local=(%d,%d) root=(%d,%d) button=%d ok=%d",
                 n, type == ButtonPress ? "ButtonPress" : "ButtonRelease",
                 windowId, targetFd, x, y, rootX, rootY, button, ok ? 1 : 0);
            {
                std::lock_guard<std::mutex> fbLock(bufferMutex);
                auto pit = popupOverlays.find(windowId);
                if (pit != popupOverlays.end()) {
                    const auto& p = pit->second;
                    LOGI("X11 vstpoc-send popup wid=0x%x display=(%d,%d) req=(%d,%d) size=%dx%d mapped=%d content=%d",
                         windowId, p.x, p.y, p.reqX, p.reqY, p.w, p.h,
                         p.mapped ? 1 : 0, p.hasContent ? 1 : 0);
                }
            }
        }
    }

    void serverLoop() {
        /* Always use TCP loopback. Our custom-built libxcb does not support
         * abstract Unix sockets — it tries filesystem path /tmp/.X11-unix/XN
         * which doesn't exist on Android. TCP 127.0.0.1:(6000+N) works reliably. */
        bool useUnix = false;

        {
            /* Try bind on basePort + N*100 increments. If port 6001 is held
             * by an orphan listener from a previous wine session (e.g. a
             * TONEX/AmpliTube wine subprocess that crashed mid-init and left
             * a stuck X11NativeDisplay behind), step up to 6101, 6201, …
             * The first port we successfully bind becomes the actual port.
             * actualPort_ is then exposed to WineHostProcess so DISPLAY=:N
             * matches the bound port, not the original requested port.
             * This is the "user shouldn't have to reboot the device" fix. */
            int basePort = kX11BasePort + displayNumber_;
            bool bound = false;
            for (int attempt = 0; attempt < 100 && running; attempt++) {
                int port = basePort + attempt * 100;
                serverFd = socket(AF_INET, SOCK_STREAM, 0);
                if (serverFd < 0) {
                    LOGE("socket failed: %s", strerror(errno));
                    return;
                }
                int one = 1;
                setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                /* DO NOT set SO_REUSEPORT. With SO_REUSEPORT the kernel
                 * lets us bind to a port already held by an orphan listener
                 * AND THEN load-balances incoming connections between us
                 * and the orphan. Wine's XOpenDisplay would land in the
                 * orphan's stuck accept backlog half the time and hang.
                 * Without SO_REUSEPORT, our bind cleanly fails with
                 * EADDRINUSE when an orphan is present, and the skip-up
                 * logic below moves on to 6101/6201/… where we get a
                 * private listener. */
                struct sockaddr_in addr = {};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(static_cast<uint16_t>(port));
                inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
                if (bind(serverFd, reinterpret_cast<struct sockaddr*>(&addr),
                         sizeof(addr)) == 0) {
                    actualPort_ = port;
                    bound = true;
                    if (attempt > 0) {
                        LOGI("X11 server bound 127.0.0.1:%d on attempt %d "
                             "(skipped %d orphan(s))",
                             port, attempt, attempt);
                    }
                    break;
                }
                LOGW("bind 127.0.0.1:%d failed: %s (will try next)",
                     port, strerror(errno));
                close(serverFd);
                serverFd = -1;
            }
            if (!bound) {
                LOGE("X11 server: exhausted bind retries; giving up");
                return;
            }
            // Bigger backlog: wine spawns ~10 processes that connect roughly
            // simultaneously (explorer, services, plugplay, wineserver, vst_host
            // and their workers). 1 is way too small — kernel rejects extras
            // and wine subprocesses fail to attach.
            if (listen(serverFd, 64) < 0) {
                LOGE("listen failed");
                close(serverFd);
                serverFd = -1;
                return;
            }
            LOGI("X11 server listening on 127.0.0.1:%d (TCP fallback)",
                 actualPort_);
        }
        listening_ = true;
        useUnixSocket_ = useUnix;

        while (running) {
            int newFd = accept(serverFd, nullptr, nullptr);
            if (newFd < 0 || !running) break;
            /* Spawn one thread per X11 connection. Wine opens ~10 connections
             * (explorer, services, plugplay, wineserver, vst_host plus
             * workers). The OLD single-threaded model serialized them:
             * client 1's request loop blocked accept() of client 2, so wine
             * subprocesses queued in the kernel and timed out → only 3 of
             * the 10 connections ever formed → wine deadlocked waiting for
             * the missing subprocesses. With thread-per-client, all 10
             * connections form concurrently — matches Java X server's
             * AcceptThread + Client.start() architecture. */
            {
                std::lock_guard<std::mutex> lk(activeClientsMutex);
                activeClientFds.push_back(newFd);
            }
            std::thread([this, newFd](){
                clientFd = newFd;  // thread_local member; per-thread fd
                /* fds are reused across connections — clear any stale queued
                 * output from a previous connection that held this fd number. */
                { std::lock_guard<std::mutex> lk(writeMapMutex); outQueue_[newFd].clear(); }

            /* Enlarge socket buffers for large GetImage replies (~6MB) */
            {
                int bufSz = 8 * 1024 * 1024;  // 8MB
                setsockopt(clientFd, SOL_SOCKET, SO_SNDBUF, &bufSz, sizeof(bufSz));
                setsockopt(clientFd, SOL_SOCKET, SO_RCVBUF, &bufSz, sizeof(bufSz));
                if (!useUnixSocket_) {
                    int flag = 1;
                    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
                }
            }
            LOGI("X11 accept: client connected (%s) fd=%d", useUnixSocket_ ? "Unix" : "TCP", clientFd);
            /* DO NOT clear windowManager_/pixmapStore_/atoms_ per connection —
             * those are server-wide state shared by all clients. Clearing them
             * here would wipe the resources allocated by other concurrent
             * wine processes. (Old single-threaded code did this safely because
             * only one client existed at a time.) */

            uint8_t req[12];
            if (!recvAll(clientFd, req, 12)) {
                LOGE("X11 accept: failed to recv 12-byte connection request");
                int fdToClose = clientFd;
                close(fdToClose);
                clientFd = -1;
                std::lock_guard<std::mutex> lk(activeClientsMutex);
                activeClientFds.erase(
                    std::remove(activeClientFds.begin(), activeClientFds.end(), fdToClose),
                    activeClientFds.end());
                return;  // exit this connection's thread
            }
            logHex("X11 client request", req, 12);
            msbFirst_ = (req[0] == 0x42);  // 0x42 = MSB first, 0x6c = LSB first
            uint16_t major = read16(req, 2);
            uint16_t minor = read16(req, 4);
            uint16_t authNameLen = read16(req, 6);
            uint16_t authDataLen = read16(req, 8);
            /* Auth name and data are each padded to 4-byte boundaries per X11 spec */
            {
                uint16_t authNamePadded = authNameLen + ((4 - (authNameLen % 4)) % 4);
                uint16_t authDataPadded = authDataLen + ((4 - (authDataLen % 4)) % 4);
                if (authNamePadded > 0) {
                    std::vector<uint8_t> skip(authNamePadded);
                    recvAll(clientFd, skip.data(), authNamePadded);
                }
                if (authDataPadded > 0) {
                    std::vector<uint8_t> skip(authDataPadded);
                    recvAll(clientFd, skip.data(), authDataPadded);
                }
            }
            sendConnectionReply();

            /* Make client socket non-blocking AFTER handshake completes.
             * The initial recv of 12-byte connection request uses blocking recvAll,
             * so O_NONBLOCK must not be set until after that succeeds. */
            {
                int flags = fcntl(clientFd, F_GETFL, 0);
                if (flags >= 0) {
                    fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);
                }
            }
            LOGI("X11 client connected (protocol %u.%u) tid=%ld — entering request loop (non-blocking)", major, minor, getTid());

            /* Reply sequence must match client expectation: 1 for first request, 2 for second, etc. (length field is not the sequence) */
            uint16_t seq = 0;
            while (running && clientFd >= 0) {
                /* Single-threaded X server architecture:
                 * This thread owns ALL X11 operations:
                 * 1. Socket read/write
                 * 2. Protocol parsing
                 * 3. Framebuffer updates
                 * 4. plugin->idle() calls (which use Xlib/XCB)
                 * 
                 * No other thread touches X11. Other threads only enqueue messages.
                 * This prevents xcb_xlib_threads_sequence_lost crashes.
                 */

                /* Step 1: Check for graceful teardown */
                if (closingGracefully.load()) {
                    if (!destroyNotifySent.load() && !childWindows.empty()) {
                        LOGI("X11 graceful teardown: sending DestroyNotify for %zu windows", childWindows.size());
                        for (uint32_t wid : childWindows) {
                            uint8_t evt[32];
                            memset(evt, 0, 32);
                            evt[0] = DestroyNotify;
                            evt[1] = 0;
                            write16(evt, 2, lastReplySeq_);
                            write32(evt, 4, wid);
                            write32(evt, 8, wid);
                            /* vstpoc 2026-05-24: per-fd mutex via sendAllLocked. */
                            sendAllLocked(clientFd, evt, 32);
                            seq++;
                            lastSeq_.store(seq, std::memory_order_relaxed);
                            lastReplySeq_ = seq;
                            {
                                std::lock_guard<std::mutex> lk(fdSeqMutex);
                                fdLastSeq[clientFd] = seq;
                            }
                        }
                        destroyNotifySent.store(true);
                        LOGI("X11 graceful teardown: DestroyNotify sent, waiting for client to disconnect");
                    }

                    auto elapsed = std::chrono::steady_clock::now() - closeStartTime;
                    if (elapsed > std::chrono::seconds(2)) {
                        LOGI("X11 graceful teardown: timeout reached (2s), forcing disconnect");
                        break;
                    }

                    usleep(10000);
                    continue;
                }

                /* Step 2: Drain touch events BEFORE polling/processing.
                 * This ensures touch input is delivered promptly even when
                 * the plugin is sending a burst of requests. */
                drainTouchQueue();

                /* Step 3: Poll socket for X11 requests with short timeout. */
                auto pollStart = std::chrono::steady_clock::now();
                struct pollfd pfd = { clientFd, POLLIN, 0 };
                int pollRet = poll(&pfd, 1, 2 /* ms */);
                if (pollRet == 0) {
                    /* No pending requests — continue to next iteration to drain more touch events */
                    continue;
                }
                if (pollRet < 0 || (pfd.revents & (POLLERR | POLLHUP))) break;

                uint8_t buf[256];
                /* Read request header (4 bytes: opcode, pad, length) */
                if (!recvAll(clientFd, buf, 4)) {
                    LOGE("X11 client disconnected tid=%ld: recv request header failed (peer closed or error)",
                         getTid());
                    break;
                }
                uint8_t opcode = buf[0];
                uint16_t length = read16(buf, 2);

                /* BigRequests: length=0 means the real 32-bit length follows in the next 4 bytes.
                 * This only happens if the client negotiated the BIG-REQUESTS extension (our
                 * QueryExtension returns "not present", so well-behaved clients won't use it).
                 * Handle defensively to avoid protocol desync. */
                if (length == 0) {
                    uint8_t extLenBuf[4];
                    if (!recvAll(clientFd, extLenBuf, 4)) break;
                    uint32_t bigLength = read32(extLenBuf, 0);
                    LOGE("X11 BigRequests: opcode=%u bigLength=%u — skipping (extension not supported)", (unsigned)opcode, bigLength);
                    if (bigLength > 2) {
                        size_t skipBytes = ((size_t)bigLength - 2) * 4; // -2 for header+extlen already consumed
                        while (skipBytes > 0) {
                            uint8_t tmp[4096];
                            size_t chunk = std::min(skipBytes, sizeof(tmp));
                            if (!recvAll(clientFd, tmp, chunk)) break;
                            skipBytes -= chunk;
                        }
                    }
                    seq++;
                    lastSeq_.store(seq, std::memory_order_relaxed);
                    /* Per-fd seq mirror + lastReplySeq_ update: events for
                     * this connection must carry a serial >= the request
                     * being processed, otherwise wine's handle_state_change
                     * rejects them as "old" and drops queued WM_STATE /
                     * focus events. Update on EVERY request (not just
                     * reply-bearing ones — lastReplySeq_ alone lags during
                     * long Configure/ChangeProperty streams). */
                    lastReplySeq_ = seq;
                    {
                        std::lock_guard<std::mutex> lk(fdSeqMutex);
                        fdLastSeq[clientFd] = seq;
                    }
                    continue;
                }

                seq++;
                lastSeq_.store(seq, std::memory_order_relaxed);
                lastReplySeq_ = seq;
                {
                    std::lock_guard<std::mutex> lk(fdSeqMutex);
                    fdLastSeq[clientFd] = seq;
                }

                thread_local int reqLogCount = 0;
                thread_local bool seenOpcode[256] = {};
                ++reqLogCount;
                if (!seenOpcode[opcode]) {
                    seenOpcode[opcode] = true;
                    LOGI("X11 opcode first seen: #%d opcode=%u %s length=%u seq=%u tid=%ld",
                         reqLogCount, (unsigned)opcode, x11OpcodeName(opcode), (unsigned)length, (unsigned)seq, getTid());
                } else if (reqLogCount <= 500) {
                    LOGI("X11 req #%d opcode=%u %s length=%u seq=%u",
                         reqLogCount, (unsigned)opcode, x11OpcodeName(opcode), (unsigned)length, (unsigned)seq);
                }

                auto reqStart = std::chrono::steady_clock::now();

                /* length is in 4-byte units; body size = length * 4 bytes */
                int bodyBytes = length * 4;

                static thread_local long long putImageRecvAccum = 0;
                if (opcode == PutImage) {
                    auto putStart = std::chrono::steady_clock::now();
                    if (!recvAll(clientFd, buf + 4, 20)) break;
                    uint32_t drawable = read32(buf, 4);
                    int w = (int)read16(buf, 12);
                    int h = (int)read16(buf, 14);
                    int x = (int)(int16_t)read16(buf, 16);
                    int y = (int)(int16_t)read16(buf, 18);
                    size_t pixelDataLen = (length >= 6) ? ((size_t)length * 4 - 24) : 0;
                    if (w > 0 && h > 0 && w <= 4096 && h <= 4096 && pixelDataLen > 0) {
                        // Reuse thread-local buffer to avoid allocation per PutImage
                        static thread_local std::vector<uint8_t> pixels;
                        if (pixels.size() < pixelDataLen) pixels.resize(pixelDataLen);
                        auto recvStart = std::chrono::steady_clock::now();
                        if (recvAll(clientFd, pixels.data(), pixelDataLen)) {
                            putImageRecvAccum += std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - recvStart).count();
                            std::lock_guard<std::mutex> lock(bufferMutex);

                            /* Determine target: framebuffer (window) or pixmap */
                            /* Check childWindows FIRST to avoid conflict with root window ID */
                            bool isWindow = false;
                            bool isTopLevel = false;
                            for (auto wid : childWindows) {
                                if (wid == drawable) { isWindow = true; break; }
                            }
                            if (!isWindow && drawable == kRootWindowId) {
                                isWindow = true;
                                isTopLevel = true;
                            }
                            if (isWindow && !childWindows.empty() && drawable == childWindows[0]) {
                                isTopLevel = true;
                            }

                            /* Skip PutImage for unmapped (hidden) windows */
                            if (isWindow && !isTopLevel && windowManager_.isUnmapped(drawable)) {
                                if (reqLogCount <= 100) LOGI("X11 PutImage SKIP unmapped wid=0x%x", drawable);
                                isWindow = false;  // suppress framebuffer write
                            }
                            /* Diagnostic: log first PutImage routings to find where editor pixels land. */
                            {
                                static thread_local int piDbg = 0;
                                bool isPopupTarget = popupOverlays.find(drawable) != popupOverlays.end();
                                if (++piDbg <= 20 || isPopupTarget) {
                                    LOGI("X11 PutImage rt: drawable=0x%x %dx%d at (%d,%d) isWindow=%d isTopLevel=%d isPopup=%d childWindows.size=%zu",
                                         drawable, w, h, x, y, isWindow?1:0, isTopLevel?1:0, isPopupTarget?1:0, childWindows.size());
                                }
                            }

                            /* Claim editor ownership for the client drawing large pixels
                             * into a window. JUCE-based plugins (BOD, etc) draw the editor
                             * into a NON-top-level child window, so the CreateWindow /
                             * ConfigureWindow heuristics that look at childWindows[0] miss
                             * them. If we see a sizable PutImage targeting any window and
                             * we don't have an owner yet, this client is it. */
                            if (isWindow && w >= 64 && h >= 64 &&
                                    editorOwnerFd.load(std::memory_order_relaxed) < 0) {
                                editorOwnerFd.store(clientFd, std::memory_order_release);
                                LOGI("X11 editor owner claimed via PutImage: fd=%d wid=0x%x %dx%d",
                                     clientFd, drawable, w, h);
                            }

                            /* Slot promotion via PutImage when a window
                             * receives a wide enough strip AND has a
                             * plausibly editor-sized tracked size. Handles
                             * plugins (X50II) that create their editor as
                             * a SEPARATE root-child window instead of a
                             * child of wine's virtual desktop — the
                             * slot-cap-at-1 CreateWindow logic would
                             * otherwise lock the wine desktop in as the
                             * slot and never see the actual editor, and
                             * touch hit-test picks the wrong window.
                             *
                             * Strip width threshold (>= 200): screens out
                             * the small 1x1 / 32x32 probe PutImages wine
                             * fires during init. Real editor paints are
                             * usually wide horizontal strips.
                             *
                             * Tracked-size check (64..4096 in both dims):
                             * the editor window itself must be plausibly
                             * sized. Throws away promotions targeting the
                             * wine desktop (typically 800x130 in our
                             * config) only if it WERE drawn into, which
                             * in practice it isn't — but the size check
                             * also blocks 65535x65535 probe windows.
                             *
                             * Only promotes ONCE (when slot is currently
                             * the desktop or empty) to avoid flipping the
                             * slot on every editor paint.
                             *
                             * Note: bufferMutex is already held here.
                             * windowManager_.getSize needs windowMapMutex. */
                            if (isWindow && w >= 200) {
                                bool isInSlot = false;
                                for (uint32_t s : pluginSlotWindows) {
                                    if (s == drawable) { isInSlot = true; break; }
                                }
                                if (!isInSlot) {
                                    int ww = 0, wh = 0;
                                    {
                                        std::lock_guard<std::mutex> mapLock(windowMapMutex);
                                        auto sz = windowManager_.getSize(drawable);
                                        ww = sz.first;
                                        wh = sz.second;
                                    }
                                    // Only promote when the new window is at
                                    // least as big as the current slot. A
                                    // modal popup (e.g. X50II's 302x308
                                    // activation dialog) drawing into its
                                    // own window must NOT shrink the
                                    // framebuffer — that clips chunks of
                                    // the larger editor's last paint to the
                                    // popup's bounds, leaving brown spots
                                    // wherever the editor doesn't repaint
                                    // (chrome/text drawn once on init).
                                    bool sizeAtLeastCurrent =
                                        (ww >= pluginWidth && wh >= pluginHeight);
                                    if (ww >= 64 && wh >= 64
                                        && ww <= 4096 && wh <= 4096
                                        && sizeAtLeastCurrent
                                        && !fbSizeFrozen) {
                                        pluginSlotWindows.clear();
                                        pluginSlotWindows.push_back(drawable);
                                        editorOwnerFd.store(clientFd,
                                            std::memory_order_release);
                                        /* Some plugins (X50II) create the
                                         * EDITOR with override_redirect=1
                                         * so the WM can't decorate it.
                                         * Our CreateWindow handler then
                                         * registers it as a popup overlay.
                                         * Now that it's promoted to a slot
                                         * (= the editor) drop it from the
                                         * popup table — otherwise our
                                         * PutImage routing keeps sending
                                         * editor pixels into a per-window
                                         * overlay buffer instead of the
                                         * framebuffer slot, and the editor
                                         * never appears. */
                                        popupOverlays.erase(drawable);
                                        constexpr uint32_t bgX11 = 0xFF302020;
                                        if (framebuffer.size() != (size_t)ww * wh) {
                                            // Preserve overlap pixels (see
                                            // setPluginSize for the why).
                                            const int oldW = pluginWidth > 0 ? pluginWidth : width;
                                            const int oldH = pluginHeight > 0 ? pluginHeight : height;
                                            std::vector<uint32_t> newFb((size_t)ww * wh, bgX11);
                                            const int copyW = std::min(oldW, ww);
                                            const int copyH = std::min(oldH, wh);
                                            if (oldW > 0 && oldH > 0 && copyW > 0 && copyH > 0 &&
                                                framebuffer.size() == (size_t)oldW * oldH) {
                                                for (int yy = 0; yy < copyH; ++yy) {
                                                    std::memcpy(&newFb[(size_t)yy * ww],
                                                                &framebuffer[(size_t)yy * oldW],
                                                                (size_t)copyW * sizeof(uint32_t));
                                                }
                                            }
                                            framebuffer = std::move(newFb);
                                        }
                                        pluginWidth = ww;
                                        pluginHeight = wh;
                                        LOGI("X11 slot promoted via PutImage: display=%d wid=0x%x window=%dx%d (strip %dx%d)",
                                             displayNumber_, drawable, ww, wh, w, h);
                                    }
                                }
                            }

                            /* Popup short-circuit: an override_redirect window
                             * draws into its own per-window backing buffer
                             * (NOT into the editor framebuffer). PutImage
                             * coords stay local to the popup — the renderer
                             * composites the buffer at the popup's root
                             * position later. If we let this fall through to
                             * the framebuffer path, the popup pixels would
                             * (a) land at root-shifted coords in the editor
                             * framebuffer and (b) get immediately overwritten
                             * by the next editor repaint, producing black
                             * rectangles at popup positions. */
                            bool isPopupDraw = false;
                            {
                                auto popIt = popupOverlays.find(drawable);
                                if (popIt != popupOverlays.end()) {
                                    isPopupDraw = true;
                                }
                            }

                            /* For child windows, offset PutImage coords by window's absolute position */
                            if (isWindow && !isTopLevel && !isPopupDraw) {
                                auto absPos = getAbsolutePos(drawable);
                                x += absPos.first;
                                y += absPos.second;
                            }

                            /* Collect mapped child window rects to clip parent drawing.
                             * On a real X11 server, child windows float above parents. On our
                             * single-framebuffer server, we simulate this by skipping parent
                             * pixels that fall within mapped child window bounds. */
                            static thread_local std::vector<ClipRect> childClip;
                            childClip.clear();
                            if (isWindow && !isPopupDraw) {
                                std::lock_guard<std::mutex> mapLock(windowMapMutex);
                                auto rects = windowManager_.getMappedChildRectsOf(drawable);
                                for (auto& r : rects) {
                                    childClip.push_back({r.x1, r.y1, r.x2, r.y2});
                                }
                                /* Also clip against mapped sibling windows above this
                                 * window in the stacking order. On a real X11 server,
                                 * higher siblings obscure lower ones. Without this,
                                 * a lower sibling's PutImage overwrites higher sibling
                                 * pixels in overlap regions. */
                                if (drawable != kRootWindowId) {
                                    auto sibRects = windowManager_.getMappedSiblingRectsAbove(drawable);
                                    for (auto& r : sibRects) {
                                        childClip.push_back({r.x1, r.y1, r.x2, r.y2});
                                    }
                                }
                            }

                            uint32_t* dstBuf = nullptr;
                            int dW = 0, dH = 0;
                            int fbw = pluginWidth > 0 ? pluginWidth : width;
                            int fbh = pluginHeight > 0 ? pluginHeight : height;
                            // For the stacked-editor layout: framebuffer is
                            // fbw × (fbh * pluginSlotWindows.size()). Look up
                            // this drawable's slot and shift y so the plugin
                            // draws into ITS slot region of the framebuffer.
                            int fbTotalH = (int)pluginSlotWindows.size() > 0
                                ? fbh * (int)pluginSlotWindows.size() : fbh;
                            bool isSlotWrite = false;
                            int slotIdx = -1;
                            if (isPopupDraw) {
                                /* Popup overlay path: write into the popup's
                                 * own backing buffer, sized to the popup. The
                                 * renderer composites this on top of the
                                 * framebuffer at popup.(x, y) every frame. */
                                auto popIt = popupOverlays.find(drawable);
                                if (popIt != popupOverlays.end()) {
                                    auto& p = popIt->second;
                                    if (p.w > 0 && p.h > 0 &&
                                        p.pixels.size() == (size_t)p.w * p.h) {
                                        dstBuf = p.pixels.data();
                                        dW = p.w;
                                        dH = p.h;
                                        p.hasContent = true;  /* wine paints here → real popup */
                                    }
                                }
                            } else if (isWindow && framebuffer.size() == (size_t)fbw * fbTotalH) {
                                dstBuf = framebuffer.data(); dW = fbw; dH = fbTotalH;
                                for (size_t i = 0; i < pluginSlotWindows.size(); ++i) {
                                    if (pluginSlotWindows[i] == drawable) { slotIdx = (int)i; break; }
                                }
                                if (slotIdx >= 0) {
                                    isSlotWrite = true;
                                    if (slotIdx > 0) y += slotIdx * fbh;
                                }
                            } else if (isWindow && framebuffer.size() == (size_t)fbw * fbh) {
                                // Single-plugin fast path (legacy).
                                dstBuf = framebuffer.data(); dW = fbw; dH = fbh;
                                if (!pluginSlotWindows.empty() && pluginSlotWindows[0] == drawable) {
                                    isSlotWrite = true;
                                    slotIdx = 0;
                                }
                            } else {
                                auto* pm = pixmapStore_.get(drawable);
                                if (pm) {
                                    dstBuf = pm->pixels.data();
                                    dW = pm->w; dH = pm->h;
                                }
                            }

                            // Track the bounding box of pixels actually written
                            // into the active slot window. AmpCraft and similar
                            // DXVK plugins declare 1290x612 via effEditGetRect
                            // but DXVK's StretchBlt presents at 896x612 (DPI
                            // downscaling in wine's vulkan_surface_presented),
                            // leaving the right 30% of the framebuffer brown.
                            // Compose polls this extent and resizes the
                            // SurfaceView to match the actually-rendered area
                            // instead of the lying declared size.
                            //
                            // Use original (pre-slot-offset) y for extent
                            // tracking: extent is per-slot, not framebuffer-y.
                            if (isSlotWrite && w > 0 && h > 0) {
                                int origY = (slotIdx > 0) ? (y - slotIdx * fbh) : y;
                                int reachX = x + w;
                                int reachY = origY + h;
                                if (drawable != renderedExtentSlot_) {
                                    renderedExtentSlot_ = drawable;
                                    renderedMaxX_ = 0;
                                    renderedMaxY_ = 0;
                                }
                                if (reachX > renderedMaxX_) renderedMaxX_ = reachX;
                                if (reachY > renderedMaxY_) renderedMaxY_ = reachY;
                            }

                            if (dstBuf) {
                                /* Detect input pixel bytes-per-pixel from total payload.
                                 * Our advertised PixmapFormat is depth=32, bpp=24 → wine
                                 * sends 3 bytes per pixel (BGR), not 4. Old code assumed
                                 * 4 bytes per pixel and read uint32 from a 3-byte stream,
                                 * producing horizontal tiling / pixel misalignment. */
                                int bytesPerPixel = 4;
                                if (pixelDataLen >= (size_t)w * h * 3 &&
                                    pixelDataLen <  (size_t)w * h * 4) {
                                    bytesPerPixel = 3;
                                }
                                /* Fast path: region fully inside destination, LSB-first, complete pixel data */
                                bool fullyCovered = (x >= 0 && y >= 0 && x + w <= dW && y + h <= dH
                                    && pixelDataLen >= (size_t)w * h * (size_t)bytesPerPixel);
                                if (fullyCovered && !msbFirst_ && childClip.empty()) {
                                    const uint8_t* src8 = pixels.data();
                                    for (int row = 0; row < h; row++) {
                                        uint32_t* dstRow = dstBuf + (y + row) * dW + x;
                                        const uint8_t* srcRow8 = src8 + (size_t)row * w * bytesPerPixel;
                                        if (bytesPerPixel == 4) {
                                            const uint32_t* srcRow32 = reinterpret_cast<const uint32_t*>(srcRow8);
                                            for (int col = 0; col < w; col++) {
                                                dstRow[col] = srcRow32[col] | 0xFF000000u;
                                            }
                                        } else {
                                            /* 24-bit BGR per pixel → assemble BGRA uint32. */
                                            for (int col = 0; col < w; col++) {
                                                uint8_t b = srcRow8[col * 3 + 0];
                                                uint8_t g = srcRow8[col * 3 + 1];
                                                uint8_t r = srcRow8[col * 3 + 2];
                                                dstRow[col] = 0xFF000000u
                                                            | ((uint32_t)r << 16)
                                                            | ((uint32_t)g <<  8)
                                                            |  (uint32_t)b;
                                            }
                                        }
                                    }
                                } else if (fullyCovered && !msbFirst_) {
                                    /* Fast path with child clipping */
                                    const uint8_t* src8 = pixels.data();
                                    for (int row = 0; row < h; row++) {
                                        int dstY = y + row;
                                        uint32_t* dstRow = dstBuf + dstY * dW + x;
                                        const uint8_t* srcRow8 = src8 + (size_t)row * w * bytesPerPixel;
                                        for (int col = 0; col < w; col++) {
                                            int dstX = x + col;
                                            bool clipped = false;
                                            for (auto& cr : childClip) {
                                                if (dstX >= cr.x1 && dstX < cr.x2 && dstY >= cr.y1 && dstY < cr.y2) {
                                                    clipped = true; break;
                                                }
                                            }
                                            if (clipped) continue;
                                            uint32_t pixel;
                                            if (bytesPerPixel == 4) {
                                                pixel = *reinterpret_cast<const uint32_t*>(srcRow8 + col * 4);
                                            } else {
                                                uint8_t b = srcRow8[col * 3 + 0];
                                                uint8_t g = srcRow8[col * 3 + 1];
                                                uint8_t r = srcRow8[col * 3 + 2];
                                                pixel = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                                            }
                                            dstRow[col] = pixel | 0xFF000000u;
                                        }
                                    }
                                } else {
                                    /* Slow path with bounds checking and child clipping.
                                     * Honors `bytesPerPixel` (3 or 4) detected above. */
                                    size_t maxPixelIdx = pixelDataLen;
                                    for (int row = 0; row < h; row++) {
                                        int dstY = y + row;
                                        if (dstY < 0 || dstY >= dH) continue;
                                        for (int col = 0; col < w; col++) {
                                            int dstX = x + col;
                                            if (dstX < 0 || dstX >= dW) continue;
                                            bool clipped = false;
                                            for (auto& cr : childClip) {
                                                if (dstX >= cr.x1 && dstX < cr.x2 && dstY >= cr.y1 && dstY < cr.y2) {
                                                    clipped = true; break;
                                                }
                                            }
                                            if (clipped) continue;
                                            size_t srcIdx = ((size_t)row * w + col) * (size_t)bytesPerPixel;
                                            if (srcIdx + (size_t)bytesPerPixel - 1 >= maxPixelIdx) continue;
                                            uint32_t pixel;
                                            if (bytesPerPixel == 3) {
                                                /* 24-bit BGR — wine's depth=32 bpp=24 format */
                                                uint8_t b = pixels[srcIdx + 0];
                                                uint8_t g = pixels[srcIdx + 1];
                                                uint8_t r = pixels[srcIdx + 2];
                                                pixel = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                                            } else if (msbFirst_) {
                                                /* MSB first: [A][R][G][B] → BGRA uint32 */
                                                uint8_t a = pixels[srcIdx], r = pixels[srcIdx+1], g = pixels[srcIdx+2], b = pixels[srcIdx+3];
                                                pixel = (a << 24) | (r << 16) | (g << 8) | b;
                                            } else {
                                                /* LSB first: [B][G][R][A] in wire — direct uint32 read */
                                                memcpy(&pixel, &pixels[srcIdx], 4);
                                            }
                                            dstBuf[(size_t)dstY * dW + dstX] = pixel | 0xFF000000u;
                                        }
                                    }
                                }
                                if (isWindow) {
                                    dirty = true;
                                    dirtyCv.notify_one();
                                }
                            }
                        }
                    } else if (pixelDataLen > 0) {
                        /* Image too large or invalid dimensions — discard pixel data */
                        std::vector<uint8_t> discard(pixelDataLen);
                        recvAll(clientFd, discard.data(), pixelDataLen);
                    }
                    {
                        auto putUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - putStart).count();
                        static thread_local long long totalPutUs = 0;
                        static thread_local int putCount = 0;
                        static thread_local auto lastPutLog = std::chrono::steady_clock::now();
                        totalPutUs += putUs;
                        putCount++;
                        auto now = std::chrono::steady_clock::now();
                        if (std::chrono::duration<double>(now - lastPutLog).count() >= 2.0) {
                            LOGI("X11Stats: PutImage %d calls in 2s, total=%lldms avg=%lldus recv=%lldms",
                                 putCount, totalPutUs / 1000, putCount > 0 ? totalPutUs / putCount : 0, putImageRecvAccum / 1000);
                            totalPutUs = 0; putCount = 0; putImageRecvAccum = 0; lastPutLog = now;
                        }
                    }
                    drainTouchQueue();  // Drain touch events before continuing
                    continue;
                }

                /* Read extra body bytes for requests that need it.
                   Some requests (like ChangeWindowAttributes, DestroyWindow) read their body inside the case handler,
                   so we skip the general read for those. */
                if (length > 0 && opcode != 2 && opcode != 4 && opcode != 38) {  /* Skip ChangeWindowAttributes (2), DestroyWindow (4), QueryPointer (38) - read body in case handlers */
                    size_t extra = (length - 1) * 4;
                    /* Cap to avoid huge allocations from bad/corrupt client */
                    if (extra > 65536) extra = 65536;
                    if (reqLogCount <= 15 && extra > 0 && extra <= 256) {
                        LOGI("X11 recv extra %zu bytes for %s", extra, x11OpcodeName(opcode));
                    }
                    if (extra > sizeof(buf) - 4) {
                        std::vector<uint8_t> discard(extra);
                        recvAll(clientFd, discard.data(), extra);
                    } else {
                        recvAll(clientFd, buf + 4, extra);
                    }
                }

                switch (opcode) {
                    case CreateWindow: {
                        if (length == 0) {
                            LOGE("X11 CreateWindow: invalid length=0 (must have body with window ID, parent, etc.)");
                            break;
                        }
                        uint32_t wid = read32(buf, 4);
                        uint32_t parentWid = read32(buf, 8);
                        int winX = (int)(int16_t)read16(buf, 12);
                        int winY = (int)(int16_t)read16(buf, 14);
                        int winWidth = (int)read16(buf, 16);
                        int winHeight = (int)read16(buf, 18);

                        /* Parse value-mask and value-list to extract
                         * override_redirect. CreateWindow body layout:
                         *   bytes 28..31  value-mask (CARD32, bit positions = CWxxx)
                         *   bytes 32..    value-list (each value 4 bytes, in
                         *                 mask-bit order, lowest bit first).
                         * CWOverrideRedirect is bit 9 (0x200). Its value is a
                         * BOOL (one byte significant, padded to 4 bytes).
                         * Counted-position approach: popcount of all set bits
                         * below bit 9 gives the slot index in the value-list. */
                        bool overrideRedirect = false;
                        if (length >= 9 /* 8+1 words = 32 bytes body */) {
                            uint32_t vmask32 = read32(buf, 28);
                            if (vmask32 & 0x200u /* CWOverrideRedirect */) {
                                uint32_t lower = vmask32 & 0x1FFu;
                                int idx = __builtin_popcount(lower);
                                int off = 32 + idx * 4;
                                if (off + 1 <= (int)sizeof(buf) &&
                                    (size_t)(off + 1) <= (size_t)length * 4) {
                                    overrideRedirect = (buf[off] != 0);
                                }
                            }
                        }

                        {
                            std::lock_guard<std::mutex> mapLock(windowMapMutex);
                            windowManager_.createWindow(wid, parentWid, winX, winY, winWidth, winHeight);
                        }
                        {
                            std::lock_guard<std::mutex> creatorLock(windowCreatorMutex);
                            windowCreator[wid] = clientFd;
                        }
                        LOGI("X11 handle CreateWindow wid=0x%x parent=0x%x pos=(%d,%d) %dx%d override_redirect=%d (childWindows size=%zu)",
                             wid, parentWid, winX, winY, winWidth, winHeight,
                             overrideRedirect ? 1 : 0, childWindows.size());

                        /* Register override_redirect popups so PutImage can
                         * route their pixels into an overlay buffer that the
                         * renderer composites on top of the editor framebuffer.
                         * Without this, popup pixels fall through to the
                         * pixmap-lookup fallback (which fails — these wids
                         * aren't pixmaps), and we paint nothing where the
                         * popup should be → black rectangle. */
                        if (overrideRedirect) {
                            std::lock_guard<std::mutex> fbLock(bufferMutex);
                            auto& p = popupOverlays[wid];
                            p.x = winX;
                            p.y = winY;
                            p.reqX = winX;
                            p.reqY = winY;
                            p.w = winWidth;
                            p.h = winHeight;
                            p.mapped = false;
                            if (winWidth > 0 && winHeight > 0) {
                                p.pixels.assign((size_t)winWidth * winHeight, 0xFF000000u);
                            }
                        }
                        // Set the framebuffer to match the LARGEST top-level window
                        // created so far. Wine creates many small probe/IME windows
                        // (1x1) before the real editor (800x130). If we lock the
                        // framebuffer to the first window (1x1), the editor never
                        // renders into anything.
                        //
                        // Two paths add a slot:
                        //   (a) The window is a direct child of root and ≥64×64
                        //       (this matches master's non-virtual-desktop layout).
                        //   (b) The window's parent is itself an existing slot
                        //       and the window is ≥64×64 — this is wine's
                        //       virtual-desktop arrangement: the wine desktop
                        //       window is a root child (slot 0) and the actual
                        //       plugin editor is its child. The real editor
                        //       *replaces* its container slots — we throw away
                        //       the wine-desktop slots so the framebuffer is
                        //       sized to the editor, not 800×(130·N) of empty
                        //       wine surfaces.
                        const int newArea = winWidth * winHeight;
                        const int curArea = pluginWidth * pluginHeight;
                        /* Reject obviously-not-an-editor sizes. Wine creates 65535×65535
                         * "as large as possible" probe windows that aren't real editors
                         * and would blow up the framebuffer to ~16 GB. */
                        const bool plausibleEditorSize =
                            (winWidth >= 64 && winHeight >= 64 &&
                             winWidth <= 4096 && winHeight <= 4096);
                        bool parentIsExistingSlot = false;
                        if (plausibleEditorSize) {
                            for (uint32_t slot : pluginSlotWindows) {
                                if (slot == parentWid) { parentIsExistingSlot = true; break; }
                            }
                        }
                        const bool isRootChildSlot =
                            (parentWid == kRootWindowId && plausibleEditorSize);
                        // Each wine subprocess hosts ONE plugin in this build,
                        // so at most one editor slot per X display. Three cases:
                        //   - parentIsExistingSlot → real plugin editor inside
                        //     wine's virtual desktop; replace the desktop slot.
                        //   - isRootChildSlot && slots empty → first candidate
                        //     (usually wine's desktop window itself, which a
                        //     later editor replaces via the case above).
                        //   - isRootChildSlot && slots not empty → wine
                        //     auxiliary windows (tooltips, IME, sysmenu).
                        //     Ignore them; without this guard each gets
                        //     appended and the framebuffer grows N× tall,
                        //     causing only the top 1/N of the editor to be
                        //     visible after letterbox.
                        const bool takeAsReplacement   = parentIsExistingSlot;
                        const bool takeAsFirstSlot     = isRootChildSlot && pluginSlotWindows.empty();
                        if (takeAsReplacement || takeAsFirstSlot) {
                            std::lock_guard<std::mutex> fbLock(bufferMutex);
                            if (takeAsReplacement) {
                                pluginSlotWindows.clear();
                            }
                            editorOwnerFd.store(clientFd, std::memory_order_release);
                            pluginSlotWindows.push_back(wid);
                            /* Slot-promoted window is the editor — must not
                             * also be tracked as a popup overlay (PutImage
                             * routing checks isPopupDraw first; if a window
                             * is in both, editor pixels would land in a per-
                             * window overlay buffer instead of the slot's
                             * region of the framebuffer and the editor
                             * never appears). See X50II behavior where the
                             * editor was created with override_redirect=1
                             * — the CreateWindow handler above added it to
                             * popupOverlays before we knew it would become
                             * a slot. */
                            popupOverlays.erase(wid);
                            // When the framebuffer is frozen by the caller
                            // (installer mode), only track the slot for
                            // touch routing — DON'T resize the framebuffer.
                            // See fbSizeFrozen comment for why.
                            if (!fbSizeFrozen) {
                                pluginWidth  = winWidth;
                                pluginHeight = winHeight;
                                constexpr uint32_t bgX11 = 0xFF302020;
                                framebuffer.assign((size_t)pluginWidth * pluginHeight, bgX11);
                            }
                            LOGI("X11: plugin slot claimed wid=0x%x %dx%d (parent=0x%x%s); framebuffer %dx%d (owner fd=%d, frozen=%d)",
                                 wid, winWidth, winHeight, parentWid,
                                 takeAsReplacement ? " replaces ancestor" : " first candidate",
                                 pluginWidth, pluginHeight, editorOwnerFd.load(),
                                 fbSizeFrozen ? 1 : 0);
                        } else if (isRootChildSlot) {
                            LOGI("X11: ignoring extra root-child wid=0x%x %dx%d (slot already claimed)",
                                 wid, winWidth, winHeight);
                        }
                        /* Send Expose for the new child window so the plugin draws even if MapWindow
                         * is never sent (e.g. due to request reorder or buffering). */
                        {
                            uint16_t evtSeq = lastReplySeq_;
                            uint8_t expose[32];
                            memset(expose, 0, 32);
                            expose[0] = Expose;
                            write16(expose, 2, evtSeq);
                            write32(expose, 4, wid);
                            write16(expose, 8, 0);
                            write16(expose, 10, 0);
                            write16(expose, 12, (uint16_t)winWidth);
                            write16(expose, 14, (uint16_t)winHeight);
                            write16(expose, 16, 0);
                            sendAllLocked(clientFd, expose, 32);
                            if (reqLogCount <= 100) LOGI("X11 CreateWindow: sent Expose for window 0x%x %dx%d", wid, winWidth, winHeight);
                        }
                        break;
                    }
                    case MapWindow: {
                        uint32_t wid = read32(buf, 4);
                        {
                            std::lock_guard<std::mutex> mapLock(windowMapMutex);
                            windowManager_.mapWindow(wid);

                            /* Raise popup subtree to front: when a top-level window
                             * (child of root, e.g. a popup menu) is mapped, move its
                             * entire subtree to the end of childWindows so the hit test
                             * (reverse iteration) finds popup windows before regular
                             * plugin widgets. */
                            size_t raised = windowManager_.raiseSubtreeToFront(wid);
                            if (raised > 0) {
                                LOGI("X11 MapWindow: raised subtree of 0x%x "
                                     "(%zu windows) to front", wid, raised);
                            }
                        }
                        /* Popup compositor: a mapped override_redirect window
                         * gets composited over the editor framebuffer until
                         * it's unmapped. The dirty flag wakes the renderer. */
                        {
                            std::lock_guard<std::mutex> fbLock(bufferMutex);
                            auto popIt = popupOverlays.find(wid);
                            if (popIt != popupOverlays.end()) {
                                popIt->second.mapped = true;
                                LOGI("X11 popup MAP wid=0x%x at (%d,%d) %dx%d",
                                     wid, popIt->second.x, popIt->second.y,
                                     popIt->second.w, popIt->second.h);
                                dirty = true;
                                dirtyCv.notify_one();
                            }
                        }
                        int expW = width, expH = height;
                        {
                            auto mapSize = windowManager_.getSize(wid);
                            if (mapSize.first > 0 && mapSize.second > 0) {
                                expW = mapSize.first;
                                expH = mapSize.second;
                            }
                        }
                        LOGI("X11 handle MapWindow wid=0x%x -> sending Expose %dx%d", wid, expW, expH);
                        uint16_t evtSeq = lastReplySeq_;
                        /* MapNotify + WM_STATE PropertyNotify. Wine's
                         * can_activate_window gates NtUserSetForegroundWindow
                         * on data->current_state.wm_state != WithdrawnState,
                         * and the only way that flips to NormalState is via
                         * a WM_STATE PropertyNotify (handle_wm_state_notify
                         * → XGetWindowProperty(WM_STATE)). On a real X11
                         * setup the WM sets this property after XMapWindow.
                         * No WM here → we set it ourselves so wine considers
                         * the window activatable for keyboard input. */
                        {
                            uint8_t mn[32];
                            memset(mn, 0, 32);
                            mn[0] = 19;  /* MapNotify */
                            write16(mn, 2, evtSeq);
                            write32(mn, 4, wid);
                            write32(mn, 8, wid);
                            mn[12] = 0;  /* override_redirect */
                            sendAllLocked(clientFd, mn, 32);
                        }
                        {
                            std::lock_guard<std::mutex> lk(wmStateMutex);
                            wmStateValues[wid] = 1;  /* NormalState */
                        }
                        {
                            uint32_t wmStateAtom = atoms_.intern("WM_STATE", false);
                            uint8_t pn[32];
                            memset(pn, 0, 32);
                            pn[0] = 28;  /* PropertyNotify */
                            write16(pn, 2, evtSeq);
                            write32(pn, 4, wid);
                            write32(pn, 8, wmStateAtom);
                            write32(pn, 12, x11Timestamp());
                            pn[16] = 0;  /* state = NewValue */
                            sendAllLocked(clientFd, pn, 32);
                        }
                        uint8_t expose[32];
                        memset(expose, 0, 32);
                        expose[0] = Expose;
                        write16(expose, 2, evtSeq);
                        write32(expose, 4, wid);
                        write16(expose, 8, 0);
                        write16(expose, 10, 0);
                        write16(expose, 12, (uint16_t)expW);
                        write16(expose, 14, (uint16_t)expH);
                        write16(expose, 16, 0);
                        sendAllLocked(clientFd, expose, 32);
                        if (reqLogCount <= 100) LOGI("X11 MapWindow: sent Expose for window 0x%x", wid);
                        break;
                    }
                    /* NOTE: X11Protocol.h defines ResizeWindow=23, but X11 opcode 23 is
                     * GetSelectionOwner (reply required). Real resize is ConfigureWindow (opcode 12).
                     * GetSelectionOwner is now handled in the generic reply block above. */
                    case PolyFillRectangle: {
                        /* Request: opcode(1), pad(1), length(2), drawable(4), gc(4),
                         * rectangles[N] of (x:int16, y:int16, w:uint16, h:uint16).
                         * Each rectangle is 8 bytes = 2 32-bit words.
                         * Header is 3 words → nrects = (length - 3) / 2. */
                        if (length < 3) break;
                        uint32_t drawable = read32(buf, 4);
                        uint32_t gcid = read32(buf, 8);
                        int nrects = ((int)length - 3) / 2;
                        if (nrects <= 0) break;
                        if (nrects > 256) nrects = 256; /* safety cap */

                        /* Look up the GC's foreground color (defaults to black). Stored
                         * as 24-bit RGB on the X11 wire; framebuffer wants ARGB with
                         * full alpha. */
                        uint32_t fgColor = 0xFF000000u;
                        {
                            std::lock_guard<std::mutex> lk(gcMutex);
                            auto it = gcForeground.find(gcid);
                            if (it != gcForeground.end()) {
                                fgColor = 0xFF000000u | (it->second & 0x00FFFFFFu);
                            }
                        }

                        std::lock_guard<std::mutex> lock(bufferMutex);

                        /* Mirror PutImage's destination resolution. */
                        bool isWindow = false;
                        bool isTopLevel = false;
                        for (auto wid : childWindows) {
                            if (wid == drawable) { isWindow = true; break; }
                        }
                        if (!isWindow && drawable == kRootWindowId) {
                            isWindow = true;
                            isTopLevel = true;
                        }
                        if (isWindow && !childWindows.empty() && drawable == childWindows[0]) {
                            isTopLevel = true;
                        }
                        if (isWindow && !isTopLevel && windowManager_.isUnmapped(drawable)) {
                            isWindow = false;
                        }

                        bool isPopupDraw = popupOverlays.find(drawable) != popupOverlays.end();

                        uint32_t* dstBuf = nullptr;
                        int dW = 0, dH = 0;
                        int fbw = pluginWidth > 0 ? pluginWidth : width;
                        int fbh = pluginHeight > 0 ? pluginHeight : height;
                        int fbTotalH = (int)pluginSlotWindows.size() > 0
                            ? fbh * (int)pluginSlotWindows.size() : fbh;
                        int slotYOffset = 0;
                        int absX = 0, absY = 0;

                        if (isPopupDraw) {
                            /* Skip popup fills too. Wine often issues PolyFillRectangle to
                             * pre-clear popup body regions before PutImageing the actual
                             * pixels. If we honor those fills (especially when the GC
                             * foreground happens to be black or some other contrasting
                             * color), we leave visible black/wrong-color blocks in regions
                             * the subsequent PutImage doesn't fully overwrite. Restoring
                             * the original no-op behavior for popup destinations matches
                             * what was on the screen before 2026-05-21. */
                            break;
                        } else if (isWindow) {
                            /* No-op for window drawables (visible framebuffer/slot). Same
                             * reasoning as above — wine's GDI uses PolyFillRectangle as a
                             * scratch background clear; honoring it overwrites pixels that
                             * weren't part of the editor's intended final image. The PolyFill
                             * destination logic below is kept for pixmap targets where the
                             * pixels are backbuffer state that must be available to a
                             * subsequent CopyArea. */
                            break;
                        } else {
                            auto* pm = pixmapStore_.get(drawable);
                            if (pm) {
                                dstBuf = pm->pixels.data();
                                dW = pm->w; dH = pm->h;
                            }
                        }

                        if (!dstBuf) break;

                        /* Same child-window clip set as PutImage uses, so we don't
                         * paint over higher children/siblings. */
                        static thread_local std::vector<ClipRect> fillClip;
                        fillClip.clear();
                        if (isWindow && !isPopupDraw) {
                            std::lock_guard<std::mutex> mapLock(windowMapMutex);
                            auto rects = windowManager_.getMappedChildRectsOf(drawable);
                            for (auto& r : rects) {
                                fillClip.push_back({r.x1, r.y1, r.x2, r.y2});
                            }
                            if (drawable != kRootWindowId) {
                                auto sibRects = windowManager_.getMappedSiblingRectsAbove(drawable);
                                for (auto& r : sibRects) {
                                    fillClip.push_back({r.x1, r.y1, r.x2, r.y2});
                                }
                            }
                        }

                        const uint8_t* rectStart = buf + 12;
                        for (int ri = 0; ri < nrects; ++ri) {
                            int rx = (int)(int16_t)read16(rectStart, ri * 8 + 0);
                            int ry = (int)(int16_t)read16(rectStart, ri * 8 + 2);
                            int rw = (int)read16(rectStart, ri * 8 + 4);
                            int rh = (int)read16(rectStart, ri * 8 + 6);
                            rx += absX;
                            ry += absY + slotYOffset;
                            int x0 = std::max(rx, 0);
                            int y0 = std::max(ry, 0);
                            int x1 = std::min(rx + rw, dW);
                            int y1 = std::min(ry + rh, dH);
                            for (int yy = y0; yy < y1; ++yy) {
                                uint32_t* row = dstBuf + (size_t)yy * dW;
                                for (int xx = x0; xx < x1; ++xx) {
                                    bool clipped = false;
                                    for (auto& cr : fillClip) {
                                        if (xx >= cr.x1 && xx < cr.x2 && yy >= cr.y1 && yy < cr.y2) {
                                            clipped = true; break;
                                        }
                                    }
                                    if (!clipped) row[xx] = fgColor;
                                }
                            }
                        }

                        if (isWindow || isPopupDraw) {
                            dirty = true;
                            dirtyCv.notify_one();
                        }
                        break;
                    }
                    case GetGeometry: {
                        /* GetGeometry request: opcode(1), unused(1), length(2), drawable(4) */
                        uint32_t drawable = read32(buf, 4);
                        /* Validate drawable exists */
                        if (drawable != kRootWindowId && !windowManager_.exists(drawable) && !pixmapStore_.exists(drawable)) {
                            sendError(9 /*BadDrawable*/, seq, drawable);
                            break;
                        }
                        /* Installer mode: when fbSizeFrozen + pluginWidth/Height
                         * are set, use those as the root-window geometry too
                         * (matches the screen size we reported in the setup
                         * reply). Otherwise wine's wizard-centering math uses
                         * the SurfaceView dimensions and the wizard lands
                         * outside the framebuffer. */
                        int geoWidth, geoHeight;
                        if (fbSizeFrozen && pluginWidth > 0 && pluginHeight > 0) {
                            geoWidth = pluginWidth;
                            geoHeight = pluginHeight;
                        } else {
                            geoWidth = width;
                            geoHeight = height;
                        }
                        const char* source = "surface-default";
                        /* Check if querying a child window with stored size */
                        {
                            auto sz = windowManager_.getSize(drawable);
                            if (sz.first > 0 && sz.second > 0) {
                                geoWidth = sz.first;
                                geoHeight = sz.second;
                                source = "windowSizes";
                            } else if (drawable == kRootWindowId && windowManager_.originalChildW() > 0) {
                                /* Return SCALED original child window size for root/parent window queries.
                                 * Plugins call XGetWindowAttributes(parentXwindow) in resize_event
                                 * and resize themselves to match. Using the ORIGINAL size (not current)
                                 * prevents a feedback loop where each resize shrinks the window further. */
                                geoWidth = (int)(windowManager_.originalChildW() * uiScale);
                                geoHeight = (int)(windowManager_.originalChildH() * uiScale);
                                if (geoWidth < 1) geoWidth = 1;
                                if (geoHeight < 1) geoHeight = 1;
                                source = "root->orig-scaled";
                            }
                        }
                        int geoX = 0, geoY = 0;
                        {
                            auto pos = windowManager_.getPosition(drawable);
                            geoX = pos.x;
                            geoY = pos.y;
                        }
                        LOGI("X11 GetGeometry drawable=0x%x -> (%d,%d) %dx%d (%s)", drawable, geoX, geoY, geoWidth, geoHeight, source);
                        sendReplyGetGeometry(seq, kRootWindowId, geoX, geoY, geoWidth, geoHeight);
                        break;
                    }
                    case GetWindowAttributes: {
                        uint32_t gwaWid = read32(buf, 4);
                        if (gwaWid != kRootWindowId && !windowManager_.exists(gwaWid)) {
                            sendError(3 /*BadWindow*/, seq, gwaWid);
                            break;
                        }
                        if (reqLogCount <= 100) LOGI("X11 handle GetWindowAttributes wid=0x%x", gwaWid);
                        /* GetWindowAttributes reply: 44 bytes total (32 header + 12 extra = reply-length 3).
                           Fields: backing_store(1), seq(2), length(4), visual(4), class(2),
                           bit_gravity(1), win_gravity(1), backing_planes(4), backing_pixel(4),
                           save_under(1), map_installed(1), map_state(1), override_redirect(1),
                           colormap(4), all_event_masks(4), your_event_masks(4),
                           do_not_propagate_mask(2), pad(2). */
                        bool isUnmapped = false;
                        {
                            std::lock_guard<std::mutex> mapLock(windowMapMutex);
                            isUnmapped = windowManager_.isUnmapped(gwaWid);
                        }
                        uint8_t reply[44];
                        memset(reply, 0, 44);
                        reply[0] = 1;  // Reply
                        reply[1] = 0;  // backing_store NotUseful
                        write16(reply, 2, seq);
                        write32(reply, 4, 3);   // length: 3 extra 4-byte units (12 bytes)
                        write32(reply, 8, kDefaultVisualId);   // visual id (must match connection setup)
                        write16(reply, 12, 1);  // class InputOutput
                        reply[14] = 0;  // bit_gravity
                        reply[15] = 0;  // win_gravity
                        write32(reply, 16, 0);  // backing_planes
                        write32(reply, 20, kBlackPixel);  // backing_pixel
                        reply[24] = 0;  // save_under
                        reply[25] = 1;  // map_installed
                        reply[26] = isUnmapped ? 0 : 2;  // map_state: 0=IsUnmapped, 2=IsViewable
                        reply[27] = 0;  // override_redirect
                        write32(reply, 28, kDefaultColormapId);  // colormap
                        write32(reply, 32, 0x00ffffff);  // all_event_masks
                        write32(reply, 36, 0x00ffffff);  // your_event_masks
                        write16(reply, 40, 0);  // do_not_propagate_mask
                        write16(reply, 42, 0);  // pad
                        sendReply(reply, 44, seq);
                        if (reqLogCount <= 100) LOGI("X11 GetWindowAttributes wid=0x%x map_state=%d", gwaWid, isUnmapped ? 0 : 2);
                        break;
                    }
                    case QueryExtension: {
                        /* QueryExtension request body (already in buf+4):
                         * bytes 4-5: name_length, bytes 6-7: pad, bytes 8+: name */
                        uint16_t nameLen = read16(buf, 4);
                        const char* extName = (nameLen > 0 && nameLen <= 200) ? reinterpret_cast<const char*>(buf + 8) : "";
                        uint8_t major = 0;  /* 0 = not present */
                        if (nameLen == 3 && strncmp(extName, "GLX", 3) == 0) {
                            major = kGLXMajorOpcode;
                        } else if (nameLen == 12 && strncmp(extName, "BIG-REQUESTS", 12) == 0) {
                            major = kBigReqMajorOpcode;
                        } else if (nameLen == 5 && strncmp(extName, "SHAPE", 5) == 0) {
                            major = kShapeMajorOpcode;
                        } else if (nameLen == 5 && strncmp(extName, "XTEST", 5) == 0) {
                            major = kXTestMajorOpcode;
                        } else if (nameLen == 8 && strncmp(extName, "Generic Event Extension", 8) == 0) {
                            /* Wine queries the long-form name. */
                            major = kGEMajorOpcode;
                        }
                        if (reqLogCount <= 50)
                            LOGI("X11 QueryExtension '%.*s' -> %s (major=%u)",
                                 (int)nameLen, extName,
                                 major ? "present" : "not present",
                                 (unsigned)major);
                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;            /* reply */
                        write16(reply, 2, seq);
                        if (major) {
                            reply[8] = 1;        /* present */
                            reply[9] = major;
                            reply[10] = 0;       /* first_event */
                            reply[11] = 0;       /* first_error */
                        }
                        sendReply(reply, 32, seq);
                        break;
                    }
                    case kBigReqMajorOpcode: {
                        /* BIG-REQUESTS Enable — has one minor-opcode (0).
                         * Reply: 32-byte standard + uint32 maximum_request_length.
                         * Java X server sends INT_MAX (0x7fffffff); wine sees
                         * that against Java and works. Our old 0x04000000
                         * was much smaller and may have caused wine to take
                         * a different code path that ends in the user32 CS
                         * deadlock. Match Java exactly. */
                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;
                        write16(reply, 2, seq);
                        write32(reply, 8, 0x7fffffffu);  /* INT_MAX, matches Java */
                        sendReply(reply, 32, seq);
                        if (reqLogCount <= 50) LOGI("X11 BIG-REQUESTS enable");
                        break;
                    }
                    case kShapeMajorOpcode: {
                        /* SHAPE — stub. Wine probes shape extension; most
                         * minor-opcodes either return nothing or a 32-byte
                         * reply with sensible defaults. Drain the body and
                         * reply with zeros for the QueryVersion-like ones. */
                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;
                        write16(reply, 2, seq);
                        /* Bytes 8-11: major/minor version (1.1) for QueryVersion */
                        write16(reply, 8, 1);
                        write16(reply, 10, 1);
                        sendReply(reply, 32, seq);
                        if (reqLogCount <= 50) LOGI("X11 SHAPE minor=%u stub", (unsigned)buf[1]);
                        break;
                    }
                    case kXTestMajorOpcode: {
                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;
                        write16(reply, 2, seq);
                        write16(reply, 8, 2);
                        write16(reply, 10, 2);
                        sendReply(reply, 32, seq);
                        if (reqLogCount <= 50) LOGI("X11 XTEST minor=%u stub", (unsigned)buf[1]);
                        break;
                    }
                    case kGEMajorOpcode: {
                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;
                        write16(reply, 2, seq);
                        write16(reply, 8, 1);
                        write16(reply, 10, 0);
                        sendReply(reply, 32, seq);
                        if (reqLogCount <= 50) LOGI("X11 GE minor=%u stub", (unsigned)buf[1]);
                        break;
                    }
                    /* --- GetImage: return framebuffer/pixmap pixel data --- */
                    case 73: { /* GetImage */
                        /* Request: opcode(1), format(1), length(2), drawable(4), x(2), y(2), w(2), h(2), plane_mask(4) */
                        uint32_t drawable = read32(buf, 4);
                        int gx = (int)(int16_t)read16(buf, 8);
                        int gy = (int)(int16_t)read16(buf, 10);
                        int gw = (int)read16(buf, 12);
                        int gh = (int)read16(buf, 14);
                        auto getImageStart = std::chrono::steady_clock::now();

                        /* Determine source pixels and copy under lock, then send without lock */
                        size_t imgBytes = (size_t)gw * gh * 4;
                        size_t imgWords = (imgBytes + 3) / 4;
                        size_t replySize = 32 + imgWords * 4;
                        // Reuse a thread-local buffer to avoid 6MB alloc+zero every frame
                        static thread_local std::vector<uint8_t> replyBuf;
                        if (replyBuf.size() < replySize) {
                            replyBuf.resize(replySize);
                        }
                        // Zero only the 32-byte header
                        std::memset(replyBuf.data(), 0, 32);
                        replyBuf[0] = 1;       // Reply
                        replyBuf[1] = 24;      // depth
                        write16(replyBuf.data(), 2, seq);
                        write32(replyBuf.data(), 4, (uint32_t)imgWords);
                        write32(replyBuf.data(), 8, 0);  // visual

                        int getImageSrcW = 0, getImageSrcH = 0;
                        bool getImageUsedShadow = false, getImageFullyCovered = false;
                        bool isWindow = false;
                        {
                            std::lock_guard<std::mutex> lock(bufferMutex);
                            const uint32_t* srcBuf = nullptr;
                            int srcW = 0, srcH = 0;
                            for (auto wid : childWindows) {
                                if (wid == drawable) { isWindow = true; break; }
                            }
                            if (!isWindow && drawable == kRootWindowId) {
                                isWindow = true;
                            }
                            bool useShadow = false;
                            if (isWindow && !framebuffer.empty()) {
                                // Framebuffer is already in X11 wire format (BGRA) — read directly
                                srcBuf = framebuffer.data();
                                useShadow = true;  // no swizzle needed
                                srcW = pluginWidth > 0 ? pluginWidth : width;
                                srcH = pluginHeight > 0 ? pluginHeight : height;
                            } else {
                                auto* pm = pixmapStore_.get(drawable);
                                if (pm) {
                                    srcBuf = pm->pixels.data();
                                    srcW = pm->w;
                                    srcH = pm->h;
                                    useShadow = true;  // Pixmaps also store X11 wire format — no swizzle needed
                                }
                            }

                            // Track for diagnostics
                            getImageSrcW = srcW; getImageSrcH = srcH;
                            getImageUsedShadow = useShadow; getImageFullyCovered = false;

                            if (srcBuf && gw > 0 && gh > 0) {
                                uint32_t* dst32 = reinterpret_cast<uint32_t*>(replyBuf.data() + 32);
                                bool fullyCovered = (gx >= 0 && gy >= 0 && gx + gw <= srcW && gy + gh <= srcH);
                                getImageFullyCovered = fullyCovered;
                                if (fullyCovered && useShadow) {
                                    // Fast path: shadow is already in X11 wire format, just memcpy rows
                                    for (int row = 0; row < gh; row++) {
                                        memcpy(dst32 + row * gw, srcBuf + (gy + row) * srcW + gx, gw * 4);
                                    }
                                } else if (fullyCovered && !msbFirst_) {
                                    for (int row = 0; row < gh; row++) {
                                        const uint32_t* srcRow = srcBuf + (gy + row) * srcW + gx;
                                        uint32_t* dstRow = dst32 + row * gw;
                                        swapRB_neon(srcRow, dstRow, gw);
                                    }
                                } else if (!msbFirst_) {
                                    // Partially covered: zero the buffer, then copy/swizzle the overlapping rows
                                    std::memset(replyBuf.data() + 32, 0, imgWords * 4);
                                    int startRow = std::max(0, -gy);
                                    int endRow = std::min(gh, srcH - gy);
                                    int startCol = std::max(0, -gx);
                                    int endCol = std::min(gw, srcW - gx);
                                    if (startRow < endRow && startCol < endCol) {
                                        int copyW = endCol - startCol;
                                        for (int row = startRow; row < endRow; row++) {
                                            const uint32_t* srcRow = srcBuf + (gy + row) * srcW + (gx + startCol);
                                            uint32_t* dstRow = dst32 + row * gw + startCol;
                                            if (useShadow) {
                                                memcpy(dstRow, srcRow, copyW * 4);
                                            } else {
                                                swapRB_neon(srcRow, dstRow, copyW);
                                            }
                                        }
                                    }
                                } else {
                                    // MSB-first slow path (rare)
                                    std::memset(replyBuf.data() + 32, 0, imgWords * 4);
                                    uint8_t* dst = replyBuf.data() + 32;
                                    for (int row = 0; row < gh; row++) {
                                        for (int col = 0; col < gw; col++) {
                                            int sx = gx + col, sy = gy + row;
                                            if (sx >= 0 && sx < srcW && sy >= 0 && sy < srcH) {
                                                uint32_t pixel = srcBuf[sy * srcW + sx];
                                                size_t off = (row * gw + col) * 4;
                                                dst[off+0] = (pixel >> 24) & 0xff;
                                                dst[off+1] = (pixel >> 0) & 0xff;
                                                dst[off+2] = (pixel >> 8) & 0xff;
                                                dst[off+3] = (pixel >> 16) & 0xff;
                                            }
                                        }
                                    }
                                }
                            } else {
                                // No source found - zero pixel data
                                std::memset(replyBuf.data() + 32, 0, imgWords * 4);
                            }
                        } /* bufferMutex released — PutImage can proceed while we send */

                        /* Compensate for integer rounding error accumulation in alpha
                         * blending. Cairo's pixman uses integer division by 255 which
                         * truncates, systematically losing ~0.5 LSB per blend cycle.
                         * Over many GetImage→composite→PutImage frames, this causes
                         * progressive darkening and dot patterns in GxPlugins.
                         *
                         * Workaround: bias each R,G,B channel up by +1 (saturating at
                         * 255) when returning pixels via GetImage. This approximately
                         * cancels the truncation loss, stabilizing pixel values across
                         * repeated blend cycles. The real fix would be implementing the
                         * RENDER extension for server-side compositing. */
                        if (isWindow && gw > 0 && gh > 0) {
                            uint32_t* dst32 = reinterpret_cast<uint32_t*>(replyBuf.data() + 32);
                            size_t totalPixels = (size_t)gw * gh;
                            for (size_t i = 0; i < totalPixels; i++) {
                                dst32[i] |= 0x00010101u;
                            }
                        }

                        auto copyDoneTime = std::chrono::steady_clock::now();
                        drainTouchQueue();
                        sendReply(replyBuf.data(), replySize, seq);
                        auto sendDoneTime = std::chrono::steady_clock::now();
                        auto copyUs = std::chrono::duration_cast<std::chrono::microseconds>(copyDoneTime - getImageStart).count();
                        auto sendUs = std::chrono::duration_cast<std::chrono::microseconds>(sendDoneTime - copyDoneTime).count();
                        if (copyUs + sendUs > 5000) {
                            LOGI("X11Perf: GetImage %dx%d copy=%lldus send=%lldus total=%lldus (src=%dx%d shadow=%d covered=%d)",
                                 gw, gh, (long long)copyUs, (long long)sendUs, (long long)(copyUs+sendUs),
                                 (int)getImageSrcW, (int)getImageSrcH, (int)getImageUsedShadow, (int)getImageFullyCovered);
                        }
                        break;
                    }
                    /* --- CreatePixmap: track offscreen pixmaps --- */
                    case 53: { /* CreatePixmap */
                        /* Request: opcode(1), depth(1), length(2), pid(4), drawable(4), width(2), height(2) */
                        uint32_t pid = read32(buf, 4);
                        int pw = (int)read16(buf, 12);
                        int ph = (int)read16(buf, 14);
                        LOGI("X11 handle CreatePixmap pid=0x%x %dx%d", pid, pw, ph);
                        pixmapStore_.create(pid, pw, ph);
                        break;
                    }
                    /* --- FreePixmap --- */
                    case 54: { /* FreePixmap */
                        uint32_t pid = read32(buf, 4);
                        LOGI("X11 handle FreePixmap pid=0x%x", pid);
                        pixmapStore_.destroy(pid);
                        break;
                    }
                    /* --- CopyArea: copy pixels between drawables --- */
                    case 62: { /* CopyArea */
                        /* Request: opcode(1), unused(1), length(2), src(4), dst(4), gc(4), src_x(2), src_y(2), dst_x(2), dst_y(2), width(2), height(2) */
                        uint32_t srcId = read32(buf, 4);
                        uint32_t dstId = read32(buf, 8);
                        int srcX = (int)(int16_t)read16(buf, 16);
                        int srcY = (int)(int16_t)read16(buf, 18);
                        int dstX = (int)(int16_t)read16(buf, 20);
                        int dstY = (int)(int16_t)read16(buf, 22);
                        int cw = (int)read16(buf, 24);
                        int ch = (int)read16(buf, 26);
                        if (reqLogCount <= 40)
                            LOGI("X11 handle CopyArea src=0x%x dst=0x%x %dx%d (%d,%d)->(%d,%d)",
                                 srcId, dstId, cw, ch, srcX, srcY, dstX, dstY);

                        /* Resolve source */
                        const uint32_t* srcPixels = nullptr;
                        int sW = 0, sH = 0;
                        bool srcIsWindow = (srcId == kRootWindowId);
                        if (!srcIsWindow) {
                            for (auto wid : childWindows) {
                                if (wid == srcId) { srcIsWindow = true; break; }
                            }
                        }

                        std::lock_guard<std::mutex> lock(bufferMutex);
                        if (srcIsWindow && !framebuffer.empty()) {
                            srcPixels = framebuffer.data();
                            sW = pluginWidth > 0 ? pluginWidth : width;
                            sH = pluginHeight > 0 ? pluginHeight : height;
                        } else {
                            auto* pm = pixmapStore_.get(srcId);
                            if (pm) {
                                srcPixels = pm->pixels.data();
                                sW = pm->w; sH = pm->h;
                            }
                        }

                        /* Resolve destination */
                        uint32_t* dstPixels = nullptr;
                        int dW = 0, dH = 0;
                        bool dstIsWindow = (dstId == kRootWindowId);
                        if (!dstIsWindow) {
                            for (auto wid : childWindows) {
                                if (wid == dstId) { dstIsWindow = true; break; }
                            }
                        }
                        if (dstIsWindow && !framebuffer.empty()) {
                            dstPixels = framebuffer.data();
                            dW = pluginWidth > 0 ? pluginWidth : width;
                            dH = pluginHeight > 0 ? pluginHeight : height;
                        } else {
                            auto* pm = pixmapStore_.get(dstId);
                            if (pm) {
                                dstPixels = pm->pixels.data();
                                dW = pm->w; dH = pm->h;
                            }
                        }

                        /* Offset child window coordinates to framebuffer coordinates */
                        if (srcIsWindow && srcId != kRootWindowId &&
                            (childWindows.empty() || srcId != childWindows[0])) {
                            auto sp = getAbsolutePos(srcId);
                            srcX += sp.first; srcY += sp.second;
                        }
                        if (dstIsWindow && dstId != kRootWindowId &&
                            (childWindows.empty() || dstId != childWindows[0])) {
                            auto dp = getAbsolutePos(dstId);
                            dstX += dp.first; dstY += dp.second;
                        }

                        /* Sibling-above clipping: when copying into a window,
                         * skip destination pixels that fall within mapped
                         * higher-stacked sibling rects. Without this, a
                         * dialog's CopyArea overwrites a popup menu that
                         * was opened on top of it (the visible bug: only the
                         * menu's right ~27px extending past the dialog show
                         * up — the rest is repeatedly overdrawn by the
                         * dialog's offscreen-pixmap → CopyArea redraw). The
                         * PutImage path already does this; CopyArea was the
                         * gap. */
                        std::vector<X11WindowManager::Rect> sibRects;
                        if (dstIsWindow && dstId != kRootWindowId) {
                            std::lock_guard<std::mutex> mapLock(windowMapMutex);
                            sibRects = windowManager_.getMappedSiblingRectsAbove(dstId);
                        }
                        auto clipped = [&](int dx, int dy) -> bool {
                            for (auto& r : sibRects) {
                                if (dx >= r.x1 && dx < r.x2 && dy >= r.y1 && dy < r.y2)
                                    return true;
                            }
                            return false;
                        };
                        if (srcPixels && dstPixels && cw > 0 && ch > 0) {
                            /* Precompute the in-bounds column span once (same for
                             * every row): the per-pixel loop's bounds checks ×
                             * 559k px cost ~18-37ms for an 860x650 full-window
                             * blit under FEX (BIAS/CEF does one per frame) and
                             * starve the X server thread → input + repaint back
                             * up. memmove whole rows; per-pixel only the rare
                             * rows a mapped sibling-above actually crosses. */
                            int c0 = 0;
                            if (-srcX > c0) c0 = -srcX;
                            if (-dstX > c0) c0 = -dstX;
                            int c1 = cw;
                            if (sW - srcX < c1) c1 = sW - srcX;
                            if (dW - dstX < c1) c1 = dW - dstX;
                            for (int row = 0; row < ch; row++) {
                                int sy = srcY + row, dy = dstY + row;
                                if (sy < 0 || sy >= sH || dy < 0 || dy >= dH) continue;
                                if (c1 <= c0) continue;
                                bool rowClipped = false;
                                for (auto& r : sibRects) {
                                    if (dy >= r.y1 && dy < r.y2) { rowClipped = true; break; }
                                }
                                if (!rowClipped) {
                                    memmove(&dstPixels[dy * dW + dstX + c0],
                                            &srcPixels[sy * sW + srcX + c0],
                                            (size_t)(c1 - c0) * sizeof(uint32_t));
                                } else {
                                    for (int col = c0; col < c1; col++) {
                                        int sx = srcX + col, dx = dstX + col;
                                        if (clipped(dx, dy)) continue;
                                        dstPixels[dy * dW + dx] = srcPixels[sy * sW + sx];
                                    }
                                }
                            }
                            if (dstIsWindow) {
                                dirty = true;
                                dirtyCv.notify_one();
                            }
                        }
                        break;
                    }
                    /* --- Requests that expect a reply (send generic 32-byte) --- */
                    case ListExtensions: {
                        /* ListExtensions reply: return GLX as available extension.
                         * Reply format: header(32) + list of STRING8 (1-byte length prefix + name).
                         * We return a single extension: "GLX" (3 chars). */
                        if (reqLogCount <= 20) LOGI("X11 handle ListExtensions -> GLX");
                        const char* extNames[] = { "GLX" };
                        const int numExt = 1;
                        /* Calculate body size: each entry = 1 byte length + N bytes name */
                        size_t bodySize = 0;
                        for (int i = 0; i < numExt; i++)
                            bodySize += 1 + strlen(extNames[i]);
                        size_t padded = (bodySize + 3) & ~3u;
                        size_t replySize = 32 + padded;
                        std::vector<uint8_t> reply(replySize, 0);
                        reply[0] = 1;  /* reply */
                        reply[1] = (uint8_t)numExt;  /* number of STRs */
                        write16(reply.data(), 2, seq);
                        write32(reply.data(), 4, (uint32_t)(padded / 4));  /* reply length in 4-byte units */
                        size_t off = 32;
                        for (int i = 0; i < numExt; i++) {
                            size_t len = strlen(extNames[i]);
                            reply[off++] = (uint8_t)len;
                            memcpy(reply.data() + off, extNames[i], len);
                            off += len;
                        }
                        sendReply(reply.data(), replySize, seq);
                        break;
                    }
                    case InternAtom: { /* 16 — must return proper atom IDs */
                        /* Request: opcode(1), only_if_exists(1), length(2), name_len(2), pad(2), name(n) */
                        uint8_t onlyIfExists = buf[1];
                        uint16_t nameLen = read16(buf, 4);
                        std::string name;
                        if (nameLen > 0 && nameLen <= 256) {
                            name.assign(reinterpret_cast<const char*>(buf + 8), nameLen);
                        }

                        uint32_t atomId = atoms_.intern(name, onlyIfExists);

                        if (reqLogCount <= 50)
                            LOGI("X11 InternAtom '%s' only_if_exists=%d -> atom=%u",
                                 name.c_str(), (int)onlyIfExists, atomId);

                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;
                        write16(reply, 2, seq);
                        write32(reply, 8, atomId);
                        sendReply(reply, 32, seq);
                        break;
                    }
                    case GetAtomName: { /* 17 — reverse lookup */
                        uint32_t atomId = read32(buf, 4);
                        std::string name = atoms_.getName(atomId);

                        /* Reply: name_length(2) at bytes 8-9, then name string */
                        uint16_t nameLen = (uint16_t)name.size();
                        uint32_t pad = (4 - (nameLen % 4)) % 4;
                        uint32_t replyLen = (nameLen + pad) / 4;
                        uint32_t replySize = 32 + nameLen + pad;
                        std::vector<uint8_t> reply(replySize, 0);
                        reply[0] = 1;
                        write16(reply.data(), 2, seq);
                        write32(reply.data(), 4, replyLen);
                        write16(reply.data(), 8, nameLen);
                        if (nameLen > 0)
                            memcpy(reply.data() + 32, name.data(), nameLen);
                        sendReply(reply.data(), replySize, seq);
                        break;
                    }
                    case 40: { /* TranslateCoordinates */
                        /* Request body (already in buf): src-window(4), dst-window(4), src-x(2), src-y(2) */
                        uint32_t srcWin = read32(buf, 4);
                        uint32_t dstWin = read32(buf, 8);
                        int16_t srcX = (int16_t)read16(buf, 12);
                        int16_t srcY = (int16_t)read16(buf, 14);

                        /* Compute absolute positions of both windows relative to root */
                        std::pair<int,int> srcAbs = {0,0}, dstAbs = {0,0};
                        {
                            std::lock_guard<std::mutex> mapLock(windowMapMutex);
                            if (srcWin != kRootWindowId) {
                                /* Include the window's own position in the chain up to root */
                                srcAbs = getAbsolutePos(srcWin);
                            }
                            if (dstWin != kRootWindowId) {
                                dstAbs = getAbsolutePos(dstWin);
                            }
                        }

                        int dstX = srcX + srcAbs.first  - dstAbs.first;
                        int dstY = srcY + srcAbs.second - dstAbs.second;

                        if (reqLogCount <= 100)
                            LOGI("X11 TranslateCoordinates src=0x%x dst=0x%x (%d,%d)->(%d,%d) [srcAbs=(%d,%d) dstAbs=(%d,%d)]",
                                 srcWin, dstWin, (int)srcX, (int)srcY, dstX, dstY,
                                 srcAbs.first, srcAbs.second, dstAbs.first, dstAbs.second);

                        /* Reply: reply(1), same-screen(1), seq(2), length(4)=0, child(4), dst-x(2), dst-y(2) */
                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;   /* Reply */
                        reply[1] = 1;   /* same-screen = True */
                        write16(reply, 2, seq);
                        write32(reply, 4, 0);   /* length = 0 */
                        write32(reply, 8, 0);   /* child = None */
                        write16(reply, 12, (uint16_t)(int16_t)dstX);
                        write16(reply, 14, (uint16_t)(int16_t)dstY);
                        sendReply(reply, 32, seq);
                        break;
                    }
                    case GetProperty: { /* 20 — minimal property store.
                                          We answer WM_STATE specially because
                                          wine's handle_wm_state_notify does
                                          XGetWindowProperty(WM_STATE) to
                                          update data->current_state.wm_state,
                                          which gates can_activate_window /
                                          NtUserSetForegroundWindow. All other
                                          properties: return "does not exist"
                                          as before. */
                        uint32_t gpWid = (length >= 6) ? read32(buf, 4) : 0;
                        uint32_t gpAtom = (length >= 6) ? read32(buf, 8) : 0;
                        std::string gpName = atoms_.getName(gpAtom);
                        if (gpName.rfind("_NET_", 0) == 0) {
                            uint64_t dpk = ((uint64_t)gpWid << 32) | gpAtom;
                            std::lock_guard<std::mutex> lk(propStoreMutex);
                            bool have = propStore_.count(dpk) > 0;
                            LOGI("X11 GetProperty %s wid=0x%x reqType=%u haveStored=%d",
                                 gpName.c_str(), gpWid, read32(buf, 12), (int)have);
                        }
                        bool isWmState = (gpName == "WM_STATE");
                        uint32_t wmStateValue = 0;
                        if (isWmState) {
                            std::lock_guard<std::mutex> lk(wmStateMutex);
                            auto it = wmStateValues.find(gpWid);
                            if (it != wmStateValues.end()) wmStateValue = it->second;
                        }
                        if (reqLogCount <= 80 || isWmState) {
                            LOGI("X11 GetProperty wid=0x%x atom=%u(%s) -> %s value=%u",
                                 gpWid, gpAtom, gpName.c_str(),
                                 (isWmState && wmStateValue != 0) ? "WM_STATE" : "none",
                                 wmStateValue);
                        }
                        if (isWmState && wmStateValue != 0) {
                            /* WM_STATE payload: state (CARD32) + icon (WINDOW),
                             * both 32-bit. value_length is in units of format/8
                             * = 4 bytes per unit for format=32 → value_length=2
                             * means 8 bytes payload. reply_length is rounded
                             * up 4-byte units, here exactly 2. */
                            uint8_t reply[40];
                            memset(reply, 0, 40);
                            reply[0] = 1;             /* reply */
                            reply[1] = 32;            /* format = 32 */
                            write16(reply, 2, seq);
                            write32(reply, 4, 2);     /* reply_length = 2 → 8
                                                       * trailing bytes after
                                                       * the 32-byte header */
                            write32(reply, 8, gpAtom);  /* type = WM_STATE atom (matches
                                                       * wine's expected type arg) */
                            write32(reply, 12, 0);    /* bytes-after = 0 */
                            write32(reply, 16, 2);    /* value_length = 2 (in 4-byte
                                                       * units for format=32) */
                            write32(reply, 32, wmStateValue);   /* state */
                            write32(reply, 36, 0);              /* icon = None */
                            sendReply(reply, 40, seq);
                        } else {
                            /* Generic property store lookup (propStore_). Echo
                             * back exactly what ChangeProperty wrote so wine's
                             * 11.9 read-back-and-confirm converges. */
                            uint32_t reqType = (length >= 6) ? read32(buf, 12) : 0;
                            uint32_t longOff = ((length >= 6) ? read32(buf, 16) : 0) * 4;  /* bytes */
                            uint32_t longLen = ((length >= 6) ? read32(buf, 20) : 0) * 4;  /* max bytes */
                            uint64_t pk = ((uint64_t)gpWid << 32) | gpAtom;
                            PropertyValue pv;
                            bool found = false;
                            {
                                std::lock_guard<std::mutex> lk(propStoreMutex);
                                auto it = propStore_.find(pk);
                                if (it != propStore_.end() && it->second.format) { pv = it->second; found = true; }
                            }
                            if (found && (reqType == 0 || reqType == pv.type)) {
                                uint32_t total = (uint32_t)pv.data.size();
                                if (longOff > total) longOff = total;
                                uint32_t unit = pv.format / 8;
                                uint32_t ret = std::min(total - longOff, longLen);
                                ret -= ret % unit;                       /* whole format units */
                                uint32_t bytesAfter = total - longOff - ret;
                                uint32_t pad = (4 - (ret & 3)) & 3;
                                std::vector<uint8_t> reply(32 + ret + pad, 0);
                                reply[0] = 1;
                                reply[1] = pv.format;
                                write16(reply.data(), 2, (uint16_t)seq);
                                write32(reply.data(), 4, (ret + pad) / 4);    /* reply_length (4-byte units) */
                                write32(reply.data(), 8, pv.type);            /* actual type */
                                write32(reply.data(), 12, bytesAfter);
                                write32(reply.data(), 16, ret / unit);        /* value_length (items) */
                                if (ret) memcpy(reply.data() + 32, pv.data.data() + longOff, ret);
                                sendReply(reply.data(), 32 + ret + pad, seq);
                                if (buf[1] /*delete*/ && bytesAfter == 0) {
                                    std::lock_guard<std::mutex> lk(propStoreMutex);
                                    propStore_.erase(pk);
                                }
                            } else {
                                /* not found, or type mismatch → empty reply (with
                                 * the actual type on mismatch, per X spec). */
                                uint8_t reply[32];
                                memset(reply, 0, 32);
                                reply[0] = 1;
                                write16(reply, 2, seq);
                                write32(reply, 4, 0);
                                if (found) write32(reply, 8, pv.type);
                                sendReply(reply, 32, seq);
                            }
                        }
                        break;
                    }
                    case 15: { /* QueryTree — root window, no children */
                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;
                        write16(reply, 2, seq);
                        write32(reply, 4, 0);     /* reply_length = 0 */
                        write32(reply, 8, kRootWindowId);   /* root */
                        /* bytes 12-15: parent = None */
                        /* bytes 16-17: num children = 0 */
                        sendReply(reply, 32, seq);
                        break;
                    }
                    case 23: { /* GetSelectionOwner — owner = None */
                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;
                        write16(reply, 2, seq);
                        /* reply_length = 0; bytes 8-11: owner = None */
                        sendReply(reply, 32, seq);
                        break;
                    }
                    case 43: { /* GetInputFocus.
                                * MUST return the ACTUAL focused window, not a
                                * constant PointerRoot. Wine's
                                * is_current_process_focused() (winex11 event.c)
                                * does XGetInputFocus + XFindContext(focus) to
                                * decide whether THIS process holds the X input
                                * focus; if we always return PointerRoot (which
                                * has no HWND context) it returns FALSE forever,
                                * and X11DRV_FocusOut then drops our foreground to
                                * the desktop (the !_NET_ACTIVE_WINDOW path) — so
                                * embedded CEF (BIAS FX 2) can never stay active
                                * and clicked <input>s never focus (no caret).
                                * Return focusedWindowId so XFindContext resolves
                                * the HWND and the process reads as focused. */
                        uint32_t f = focusedWindowId.load(std::memory_order_acquire);
                        /* 0x8 = revert GetInputFocus to PointerRoot (pre-cont.7) */
                        if (focusModeMask_.load(std::memory_order_relaxed) & 0x8u) f = 0;
                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;
                        reply[1] = 1;            /* revert_to = PointerRoot */
                        write16(reply, 2, seq);
                        write32(reply, 8, f ? f : 1);  /* focus = focused window, else PointerRoot */
                        sendReply(reply, 32, seq);
                        break;
                    }
                    case 44: { /* QueryKeymap — 32 bytes of zero (no keys pressed) */
                        uint8_t reply[40];
                        memset(reply, 0, 40);
                        reply[0] = 1;
                        write16(reply, 2, seq);
                        write32(reply, 4, 2);    /* reply_length = 2 (8 bytes of the 32 keymap bytes already in fixed area) */
                        sendReply(reply, 40, seq);
                        break;
                    }
                    case 84: { /* AllocColor — return black */
                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;
                        write16(reply, 2, seq);
                        /* bytes 8-9: red, 10-11: green, 12-13: blue — all 0 */
                        write32(reply, 16, kBlackPixel);    /* pixel = 0 */
                        sendReply(reply, 32, seq);
                        break;
                    }
                    case 26: { /* GrabPointer — store grab + reply Success(0).
                                * Wine grabs with owner_events=FALSE for menu
                                * mode; tracking the grab window lets us deliver
                                * outside-clicks to a JUCE menu so it dismisses
                                * (see drainTouchQueue + the xPointerGrab fields).
                                * Request: opcode(1), owner-events(1)=buf[1],
                                * length(2), grab-window(4)=buf[4..7]. */
                        bool ownerEvents = (buf[1] != 0);
                        uint32_t grabWin = read32(buf, 4);
                        xPointerGrabWindow_.store(grabWin, std::memory_order_release);
                        xPointerGrabOwnerEvents_.store(ownerEvents, std::memory_order_release);
                        if (reqLogCount <= 500) {
                            LOGI("X11 GrabPointer: grab-window=0x%x owner-events=%d "
                                 "(outside-click->grab routing %s)",
                                 grabWin, ownerEvents ? 1 : 0, ownerEvents ? "OFF" : "ON");
                        }
                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;   /* reply */
                        reply[1] = 0;   /* status = Success */
                        write16(reply, 2, seq);
                        sendReply(reply, 32, seq);
                        break;
                    }
                    case 27: { /* UngrabPointer — clear grab (no reply). */
                        uint32_t prevGrab = xPointerGrabWindow_.exchange(0, std::memory_order_acq_rel);
                        xPointerGrabOwnerEvents_.store(false, std::memory_order_release);
                        if (prevGrab && reqLogCount <= 500) {
                            LOGI("X11 UngrabPointer: cleared grab-window=0x%x", prevGrab);
                        }
                        break;
                    }
                    case 21:  /* ListProperties — empty */
                    case 31:  /* GrabKeyboard — Success(0) */
                    case 39:  /* GetMotionEvents — empty */
                    case 47:  /* QueryFont — empty (no font loaded) */
                    case 49:  /* ListFonts — empty */
                    case 52:  /* GetFontPath — empty */
                    case 83:  /* ListInstalledColormaps — empty */
                    case 91:  /* QueryColors — empty */
                    case 97: { /* QueryBestSize — return 0x0 */
                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;
                        write16(reply, 2, seq);
                        /* reply_length = 0 — clean 32-byte reply, no trailing data */
                        sendReply(reply, 32, seq);
                        break;
                    }
                    case 119: { /* GetModifierMapping — return Java's exact
                                   8-modifier × 8-keycode table. With kpm=2
                                   wine deadlocks during user32 keyboard
                                   init; Java's kpm=8 + real Shift/Ctrl/Alt
                                   keycodes lets wine progress.
                                   Captured from au.com.darkside.xserver. */
                        const uint8_t kpm = 8;
                        const uint32_t dataWords = (uint32_t)kpm * 2;  // 16 words = 64 bytes
                        const size_t dataBytes = (size_t)dataWords * 4;
                        std::vector<uint8_t> reply(32 + dataBytes, 0);
                        reply[0] = 1;
                        reply[1] = kpm;
                        write16(reply.data(), 2, seq);
                        write32(reply.data(), 4, dataWords);
                        /* Java's modifier table: 8 slots × 8 keycodes each.
                         * Slot 0 (Shift):   keycodes 60, 61
                         * Slot 1 (Lock):    empty
                         * Slot 2 (Control): keycodes 114, 115
                         * Slot 3 (Mod1):    keycodes 58, 59
                         * Slot 4 (Mod2):    empty
                         * Slot 5 (Mod3):    empty
                         * Slot 6 (Mod4):    keycodes 118, 119
                         * Slot 7 (Mod5):    empty                            */
                        static const uint8_t kJavaModifierTable[64] = {
                            0x3c, 0x3d, 0,0,0,0,0,0,    // Shift
                            0,    0,    0,0,0,0,0,0,    // Lock
                            0x72, 0x73, 0,0,0,0,0,0,    // Control
                            0x3a, 0x3b, 0,0,0,0,0,0,    // Mod1
                            0,    0,    0,0,0,0,0,0,    // Mod2
                            0,    0,    0,0,0,0,0,0,    // Mod3
                            0x76, 0x77, 0,0,0,0,0,0,    // Mod4
                            0,    0,    0,0,0,0,0,0,    // Mod5
                        };
                        memcpy(reply.data() + 32, kJavaModifierTable, sizeof(kJavaModifierTable));
                        sendReply(reply.data(), reply.size(), seq);
                        break;
                    }
                    case 102: /* GetKeyboardMapping */
                    case 103: /* GetKeyboardControl */
                    case 104: /* GetPointerControl */
                    case 106: /* GetPointerMapping */
                    case 116: /* SetPointerMapping (has 1-byte reply) */
                    case 118: /* SetModifierMapping (has 1-byte reply) */ {
                        if (reqLogCount <= 50) LOGI("X11 handle %s opcode=%u (generic 32-byte reply)", x11OpcodeName(opcode), (unsigned)opcode);
                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;
                        write16(reply, 2, seq);
                        sendReply(reply, 32, seq);
                        break;
                    }
                    case 38: { /* QueryPointer - needed for plugin to track mouse position */
                        if (reqLogCount <= 20) LOGI("X11 handle QueryPointer");
                        /* Read the body: window(4). length=2 so body = (2-1)*4 = 4 bytes */
                        {
                            int qpBody = (length > 1) ? (int)(length - 1) * 4 : 0;
                            if (qpBody > 0) {
                                std::vector<uint8_t> qpBuf(qpBody);
                                if (!recvAll(clientFd, qpBuf.data(), qpBody)) break;
                            }
                        }
                        /* vstpoc 2026-05-25: QueryPointer cache REVERTED.
                         * Triggered libxcb assertion
                         * "xcb_xlib_threads_sequence_lost" — the cached
                         * reply's sequence-number overwrite races with
                         * xcb's expected-reply counter and desyncs. Fix
                         * would need to also fence/serialize via fdSeqMutex
                         * but the saving (~3% CPU) isn't worth the risk
                         * to all wine→X11 traffic. Drop the cache;
                         * always recompute. */
                        /* Build proper QueryPointer reply:
                         * byte 0: reply type (1)
                         * byte 1: same-screen boolean (1 = true)
                         * bytes 2-3: sequence number
                         * bytes 4-7: reply length (0 for QueryPointer)
                         * bytes 8-11: root window
                         * bytes 12-15: child window (0 = none)
                         * bytes 16-17: root-x
                         * bytes 18-19: root-y
                         * bytes 20-21: win-x
                         * bytes 22-23: win-y
                         * bytes 24-25: mask (button state)
                         */
                        /* lastPointer is in framebuffer (displayed) coords.
                         * Wine's menu modal loop calls GetCursorPos and
                         * tests against the popup window's screen rect —
                         * but wine's view of the popup is at its REQUESTED
                         * (pre-flip) position, not where we composite it.
                         * If we return framebuffer coords, wine sees the
                         * cursor as outside the popup whenever we
                         * smart-flipped it, and the menu dismisses itself
                         * after the first click. Apply the inverse-flip
                         * for the topmost mapped popup containing the
                         * cursor so wine's hit-test matches its own
                         * coordinate system. */
                        int qpFbX = lastPointerX.load(std::memory_order_relaxed);
                        int qpFbY = lastPointerY.load(std::memory_order_relaxed);
                        int qpWineX = qpFbX;
                        int qpWineY = qpFbY;
                        {
                            std::lock_guard<std::mutex> fbLock(bufferMutex);
                            const auto& cws = windowManager_.childWindows();
                            for (auto it = cws.rbegin(); it != cws.rend(); ++it) {
                                auto pit = popupOverlays.find(*it);
                                if (pit == popupOverlays.end()) continue;
                                const auto& p = pit->second;
                                if (!isRenderablePopup(p)) continue;
                                if (qpFbX >= p.x && qpFbX < p.x + p.w &&
                                    qpFbY >= p.y && qpFbY < p.y + p.h) {
                                    qpWineX = p.reqX + (qpFbX - p.x);
                                    qpWineY = p.reqY + (qpFbY - p.y);
                                    break;
                                }
                            }
                        }
                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;  /* reply */
                        reply[1] = 1;  /* same-screen = true */
                        write16(reply, 2, seq);
                        write32(reply, 8, kRootWindowId);
                        write16(reply, 16, static_cast<uint16_t>(qpWineX));
                        write16(reply, 18, static_cast<uint16_t>(qpWineY));
                        write16(reply, 20, static_cast<uint16_t>(qpWineX));
                        write16(reply, 22, static_cast<uint16_t>(qpWineY));
                        uint16_t mask = 0;
                        if (pointerButton1Down.load(std::memory_order_relaxed)) mask |= (1 << 8); /* Button1Mask */
                        if (pointerButton3Down.load(std::memory_order_relaxed)) mask |= (1 << 10); /* Button3Mask */
                        write16(reply, 24, mask);
                        if (mask || qpWineX != qpFbX || qpWineY != qpFbY) {
                            static std::atomic<uint32_t> qpDbg{0};
                            uint32_t n = ++qpDbg;
                            if (n <= 120 || n % 50 == 0) {
                                LOGI("X11 QueryPointer reply #%u fb=(%d,%d) wine=(%d,%d) mask=0x%x",
                                     n, qpFbX, qpFbY, qpWineX, qpWineY, mask);
                            }
                        }
                        sendReply(reply, 32, seq);
                        break;
                    }
                    case 2: { /* ChangeWindowAttributes */
                        /* Request format: opcode(1), pad(1), length(2), window(4), value_mask(4), then attributes */
                        /* length is total request size in 4-byte units (including 4-byte header) */
                        /* Body = (length - 1) * 4 bytes */
                        int cwBodyBytes = (length > 1) ? (int)(length - 1) * 4 : 0;
                        if (cwBodyBytes < 8) {
                            LOGE("X11 ChangeWindowAttributes: body too small %d (length=%u)", cwBodyBytes, (unsigned)length);
                            if (cwBodyBytes > 0) {
                                std::vector<uint8_t> skip(cwBodyBytes);
                                recvAll(clientFd, skip.data(), cwBodyBytes);
                            }
                            break;
                        }
                        if (cwBodyBytes > 256) {
                            LOGE("X11 ChangeWindowAttributes: body too large %d (length=%u), skipping", cwBodyBytes, (unsigned)length);
                            std::vector<uint8_t> skip(cwBodyBytes);
                            recvAll(clientFd, skip.data(), cwBodyBytes);
                            break;
                        }
                        if (!recvAll(clientFd, buf + 4, cwBodyBytes)) {
                            LOGE("X11 ChangeWindowAttributes: failed to read body (%d bytes)", cwBodyBytes);
                            break;
                        }
                        uint32_t window = read32(buf, 4);
                        uint32_t valueMask = read32(buf, 8);
                        LOGI("X11 handle ChangeWindowAttributes window=0x%x value_mask=0x%x length=%u bodyBytes=%d", window, valueMask, (unsigned)length, cwBodyBytes);

                        /* Parse attributes in bit order (lowest bit first).
                         * Attributes start at buf[12] (after header[4] + window[4] + valueMask[4]). */
                        int attrOffset = 12;
                        for (int bit = 0; bit < 32 && attrOffset + 4 <= (int)(4 + cwBodyBytes); ++bit) {
                            if (valueMask & (1U << bit)) {
                                if (bit == 9) {  /* CWOverrideRedirect */
                                    bool ovr = (buf[attrOffset] != 0);
                                    LOGI("X11 ChangeWindowAttributes: window=0x%x override_redirect=%d",
                                         window, ovr ? 1 : 0);
                                    /* Wine transitions windows between managed
                                     * and unmanaged via this attribute. If the
                                     * window is being turned INTO an override-
                                     * redirect popup, register it in our
                                     * overlay table so the compositor picks
                                     * it up. If being turned OUT of override
                                     * mode, drop it from overlays so it
                                     * renders normally. */
                                    std::lock_guard<std::mutex> fbLock(bufferMutex);
                                    if (ovr) {
                                        auto sz = windowManager_.getSize(window);
                                        auto pos = windowManager_.getPosition(window);
                                        auto& p = popupOverlays[window];
                                        p.x = pos.x;
                                        p.y = pos.y;
                                        p.w = sz.first;
                                        p.h = sz.second;
                                        p.mapped = !windowManager_.isUnmapped(window);
                                        if (p.w > 0 && p.h > 0) {
                                            p.pixels.assign((size_t)p.w * p.h, 0xFF000000u);
                                        }
                                    } else {
                                        popupOverlays.erase(window);
                                    }
                                } else if (bit == 11) {  /* CWEventMask */
                                    uint32_t eventMask = read32(buf, attrOffset);
                                    windowManager_.setEventMask(window, eventMask);
                                    LOGI("X11 ChangeWindowAttributes: window=0x%x event_mask=0x%x", window, eventMask);
                                }
                                attrOffset += 4;
                            }
                        }
                        break;
                    }
                    /* --- Void requests (no reply expected by client) --- */
                    case 4:  /* DestroyWindow */
                        if (!recvAll(clientFd, buf + 4, 4)) break;
                        {
                            uint32_t window = read32(buf, 4);
                            {
                                std::lock_guard<std::mutex> mapLock(windowMapMutex);
                                windowManager_.destroyWindow(window);
                            }
                            {
                                std::lock_guard<std::mutex> fbLock(bufferMutex);
                                if (popupOverlays.erase(window)) {
                                    dirty = true;
                                    dirtyCv.notify_one();
                                }
                            }
                            /* Defensive: if the grab window is destroyed without a
                             * preceding UngrabPointer, drop the stale grab so we
                             * don't keep routing clicks to a dead window. */
                            uint32_t expectGrab = window;
                            if (xPointerGrabWindow_.compare_exchange_strong(
                                    expectGrab, 0, std::memory_order_acq_rel)) {
                                xPointerGrabOwnerEvents_.store(false, std::memory_order_release);
                            }
                            if (reqLogCount <= 15) LOGI("X11 handle DestroyWindow window=0x%x (cleaned up)", window);
                        }
                        break;
                    case SendEvent: {
                        /* SendEvent: opcode(1), propagate(1), length(2), destination(4), event_mask(4), event(32)
                         * Body already read: buf[4..7]=destination, buf[8..11]=event_mask, buf[12..43]=event */
                        uint32_t seDest   = read32(buf, 4);
                        uint8_t  seEvType = buf[12] & 0x7f;
                        /* EWMH window-manager role: wine activates a window by
                         * XSendEvent-ing a _NET_ACTIVE_WINDOW ClientMessage to the
                         * root (set_net_active_window, winex11 window.c). As the
                         * "WM" we consume it and reflect the active window back via
                         * the root's _NET_ACTIVE_WINDOW property + a PropertyNotify
                         * so wine reconciles its pending-activate state. Advertising
                         * + honoring this puts wine on its EWMH activation path
                         * (vs the no-WM fallback), which is what properly activates
                         * embedded CEF editor WebContents (BIAS FX 2) so a clicked
                         * <input> focuses. The X input focus itself still flows
                         * through wine's own XSetInputFocus (SetInputFocus handler). */
                        if (seEvType == 33 /*ClientMessage*/ && seDest == kRootWindowId) {
                            uint32_t seMsgType = read32(buf, 20);   /* event message_type (atom) */
                            static int cmDbg = 0;
                            if (cmDbg++ < 50)
                                LOGI("X11 ClientMessage->root type=%u(%s) win=0x%x",
                                     seMsgType, atoms_.getName(seMsgType).c_str(), read32(buf, 16));
                            uint32_t netActiveWindow = atoms_.intern("_NET_ACTIVE_WINDOW", true);
                            if (netActiveWindow && seMsgType == netActiveWindow) {
                                uint32_t target = read32(buf, 16);  /* event window = window to activate */
                                {
                                    std::lock_guard<std::mutex> lk(propStoreMutex);
                                    auto& pv = propStore_[((uint64_t)kRootWindowId << 32) | netActiveWindow];
                                    pv.type = 33 /*XA_WINDOW*/; pv.format = 32;
                                    pv.data.assign(4, 0);
                                    pv.data[0] = (uint8_t)(target & 0xff);
                                    pv.data[1] = (uint8_t)((target >> 8) & 0xff);
                                    pv.data[2] = (uint8_t)((target >> 16) & 0xff);
                                    pv.data[3] = (uint8_t)((target >> 24) & 0xff);
                                }
                                uint8_t pn[32];
                                memset(pn, 0, 32);
                                pn[0] = 28; /* PropertyNotify */
                                write16(pn, 2, lastReplySeq_);
                                write32(pn, 4, kRootWindowId);
                                write32(pn, 8, netActiveWindow);
                                write32(pn, 12, x11Timestamp());
                                pn[16] = 0; /* state = NewValue */
                                sendAllLocked(clientFd, pn, 32);
                                LOGI("X11 _NET_ACTIVE_WINDOW: activate target=0x%x", target);
                                break;  /* consumed by the WM — do not echo back */
                            }
                        }
                        /* Default: forward the event back to the client (set bit 7
                         * = "sent via SendEvent"; rewrite seq to match XCB). */
                        if (reqLogCount <= 30) LOGI("X11 handle SendEvent (forwarding 32-byte event to client)");
                        write16(buf + 12, 2, lastReplySeq_);
                        buf[12] |= 0x80;
                        sendAllLocked(clientFd, buf + 12, 32);
                        break;
                    }
                    case 10: { /* UnmapWindow (opcode 10) */
                        uint32_t umWid = read32(buf, 4);
                        {
                            std::lock_guard<std::mutex> mapLock(windowMapMutex);
                            windowManager_.unmapWindow(umWid);
                        }
                        /* Flip WM_STATE back to WithdrawnState + PropertyNotify
                         * so wine clears current_state.wm_state via
                         * handle_wm_state_notify. Mirrors the MapWindow path. */
                        {
                            std::lock_guard<std::mutex> lk(wmStateMutex);
                            wmStateValues[umWid] = 0;  /* WithdrawnState */
                        }
                        {
                            uint32_t wmStateAtom = atoms_.intern("WM_STATE", false);
                            uint16_t evtSeq = lastReplySeq_;
                            uint8_t pn[32];
                            memset(pn, 0, 32);
                            pn[0] = 28;  /* PropertyNotify */
                            write16(pn, 2, evtSeq);
                            write32(pn, 4, umWid);
                            write32(pn, 8, wmStateAtom);
                            write32(pn, 12, x11Timestamp());
                            pn[16] = 0;
                            sendAllLocked(clientFd, pn, 32);
                        }
                        /* Popup overlay goes invisible. The framebuffer
                         * beneath stays as-is; wine usually sends Expose to
                         * the editor when the popup goes away, but the
                         * editor's repaint targets the framebuffer, not the
                         * popup, so the popup's pixels simply stop being
                         * composited next frame. */
                        {
                            std::lock_guard<std::mutex> fbLock(bufferMutex);
                            auto popIt = popupOverlays.find(umWid);
                            if (popIt != popupOverlays.end()) {
                                popIt->second.mapped = false;
                                LOGI("X11 popup UNMAP wid=0x%x", umWid);
                                dirty = true;
                                dirtyCv.notify_one();
                            }
                        }
                        /* Send Expose to the top-level plugin window so it repaints
                           over the now-hidden window's stale pixels in the framebuffer.
                           Only do this for the popup-level window itself (not its children)
                           to avoid an event flood from recursive widget_hide. */
                        if (!childWindows.empty() && umWid != childWindows[0]) {
                            /* Check if this is a top-level popup (parent is root) */
                            auto umPos = windowManager_.getPosition(umWid);
                            if (umPos.parent == kRootWindowId) {
                                uint32_t topWin = childWindows[0];
                                auto topSz = windowManager_.getSize(topWin);
                                if (topSz.first > 0 && topSz.second > 0) {
                                    uint16_t evtSeq = lastReplySeq_;
                                    uint8_t evt[32];
                                    memset(evt, 0, 32);
                                    evt[0] = Expose;
                                    write16(evt, 2, evtSeq);
                                    write32(evt, 4, topWin);
                                    write16(evt, 8, 0);
                                    write16(evt, 10, 0);
                                    write16(evt, 12, (uint16_t)topSz.first);
                                    write16(evt, 14, (uint16_t)topSz.second);
                                    write16(evt, 16, 0);
                                    sendAllLocked(clientFd, evt, 32);
                                    LOGI("X11 UnmapWindow 0x%x: sent Expose to main window 0x%x", umWid, topWin);
                                }
                            }
                        }
                        break;
                    }
                    case 5:  /* DestroySubwindows */
                    case 9:  /* MapSubwindows */
                    case 12: { /* ConfigureWindow — may resize the plugin window */
                        /* Request layout: opcode(1) unused(1) length(2) window(4) value-mask(2) pad(2) values... */
                        uint32_t cfgWid = read32(buf, 4);
                        uint16_t vmask  = read16(buf, 8);  /* value-mask: which fields are being set */
                        /* Values follow in bit-order (lowest bit first): X,Y,W,H,BorderWidth,Sibling,StackMode */
                        int valOff = 12;  /* first value starts at byte 12 */
                        /* Extract X (bit0) and Y (bit1) and update window position.
                         * Track if position actually changed — we need to send Expose
                         * so the plugin redraws at the new location (our server uses a
                         * single framebuffer and can't relocate pixels like a real X server). */
                        bool posChanged = false;
                        uint32_t exposeTopWid = 0;
                        int exposeTopW = 0, exposeTopH = 0;
                        int newXLocal = INT_MIN, newYLocal = INT_MIN;
                        {
                            std::lock_guard<std::mutex> mapLock(windowMapMutex);
                            if (vmask & 0x0001) {
                                int newX = (int)(int32_t)read32(buf, valOff); valOff += 4;
                                newXLocal = newX;
                                if (windowManager_.setPositionX(cfgWid, newX))
                                    posChanged = true;
                            }
                            if (vmask & 0x0002) {
                                int newY = (int)(int32_t)read32(buf, valOff); valOff += 4;
                                newYLocal = newY;
                                if (windowManager_.setPositionY(cfgWid, newY))
                                    posChanged = true;
                            }
                            /* Capture top-level window info for Expose if position changed */
                            if (posChanged && !childWindows.empty()) {
                                exposeTopWid = childWindows[0];
                                auto topSz = windowManager_.getSize(exposeTopWid);
                                exposeTopW = topSz.first;
                                exposeTopH = topSz.second;
                            }
                        }
                        int newW = -1, newH = -1;
                        if ((vmask & 0x0004) && valOff + 4 <= (int)sizeof(buf)) { newW = (int)read32(buf, valOff); valOff += 4; }
                        if ((vmask & 0x0008) && valOff + 4 <= (int)sizeof(buf)) { newH = (int)read32(buf, valOff); valOff += 4; }

                        bool isChildWin = false;
                        int finalW = -1, finalH = -1;
                        bool sizeChanged = false;
                        {
                            std::lock_guard<std::mutex> mapLock(windowMapMutex);
                            isChildWin = windowManager_.exists(cfgWid);

                            if (isChildWin && (newW > 0 || newH > 0)) {
                                /* Compute final dimensions */
                                auto curSize = windowManager_.getSize(cfgWid);
                                finalW = (newW > 0) ? newW : curSize.first;
                                finalH = (newH > 0) ? newH : curSize.second;

                                /* Only act on actual size changes to avoid flicker from repeated
                                 * same-size ConfigureWindow calls (plugins often send these every idle). */
                                sizeChanged = (cfgWid == childWindows[0])
                                    ? (finalW != pluginWidth || finalH != pluginHeight)
                                    : (finalW != curSize.first || finalH != curSize.second);

                                windowManager_.setSize(cfgWid, finalW, finalH);
                            }
                        }

                        /* Update popup overlay geometry if this is one. Wine
                         * typically positions a popup with ConfigureWindow
                         * right before mapping it.
                         *
                         * Smart flip: if the requested popup position would
                         * extend BELOW the editor framebuffer (typical with
                         * combo dropdowns clicked near the bottom of the
                         * editor — wine places the menu below the trigger),
                         * relocate it ABOVE the trigger instead so the
                         * whole popup fits on-screen. This is what Windows
                         * does natively. Also patches windowManager_'s
                         * position so touch hit-test routes the shifted
                         * popup correctly. */
                        bool popupShifted = false;
                        bool popupShiftedXFlag = false;
                        bool popupShiftedYFlag = false;
                        int popupShiftedX = 0;
                        int popupShiftedY = 0;
                        {
                            std::lock_guard<std::mutex> fbLock(bufferMutex);
                            auto popIt = popupOverlays.find(cfgWid);
                            if (popIt != popupOverlays.end()) {
                                auto& p = popIt->second;
                                /* Update wine-requested coords ONLY for axes
                                 * this request actually carries. Then flip
                                 * computes from reqX/reqY — so a second
                                 * ConfigureWindow with X+W+H but no Y still
                                 * uses the original Y as the flip input.
                                 * Without this, p.y (already flipped) would
                                 * become the new input and we'd re-flip. */
                                if (newXLocal != INT_MIN || newYLocal != INT_MIN) {
                                    auto popAbsPos = windowManager_.getAbsolutePos(cfgWid);
                                    if (newXLocal != INT_MIN) p.reqX = popAbsPos.first;
                                    if (newYLocal != INT_MIN) p.reqY = popAbsPos.second;
                                }
                                int desiredX = p.reqX;
                                int desiredY = p.reqY;
                                if (finalW > 0 && finalH > 0 &&
                                    (finalW != p.w || finalH != p.h)) {
                                    p.w = finalW;
                                    p.h = finalH;
                                    p.pixels.assign((size_t)finalW * finalH, 0xFF000000u);
                                }
                                /* Smart-flip computed from desiredX/Y (wine's
                                 * requested coord), not from p.x/y. Same input
                                 * → same output. Wine's view of the popup's
                                 * position stays at desiredX/Y; the overlay's
                                 * displayed position lands at p.x/y. */
                                const int fbW = pluginWidth > 0 ? pluginWidth : 0;
                                const int fbH = pluginHeight > 0 ? pluginHeight : 0;
                                int actualX = desiredX;
                                int actualY = desiredY;
                                if (fbH > 0 && p.h > 0 && p.h < fbH &&
                                    desiredY + p.h > fbH) {
                                    int flipped = desiredY - p.h;
                                    if (flipped < 0) {
                                        flipped = std::max(0, fbH - p.h);
                                    }
                                    actualY = flipped;
                                }
                                if (fbW > 0 && p.w > 0 && p.w < fbW &&
                                    desiredX + p.w > fbW) {
                                    int flippedX = desiredX - p.w;
                                    if (flippedX < 0) {
                                        flippedX = std::max(0, fbW - p.w);
                                    }
                                    actualX = flippedX;
                                }
                                /* Sub-popups are positioned relative to their
                                 * parent's wine-side (pre-flip) coord, so they
                                 * can land entirely below or right of the
                                 * framebuffer when the parent was flipped.
                                 * The flip-above branch only triggers when
                                 * desiredY+h overflows; if desiredY itself is
                                 * already off-screen (e.g. wine asks for
                                 * y=660 on a 549-tall framebuffer because the
                                 * parent popup is at wine y=528), flipped =
                                 * 660-94 = 566 is still off-screen. Final
                                 * clamp: pin the popup to fit the viewport. */
                                if (fbH > 0 && p.h > 0 && actualY + p.h > fbH) {
                                    actualY = std::max(0, fbH - p.h);
                                }
                                if (fbW > 0 && p.w > 0 && actualX + p.w > fbW) {
                                    actualX = std::max(0, fbW - p.w);
                                }
                                if ((actualX != desiredX || actualY != desiredY) &&
                                    (actualX != p.x || actualY != p.y)) {
                                    LOGI("X11 popup flip wid=0x%x %dx%d (%d,%d) -> (%d,%d) (fb=%dx%d)",
                                         cfgWid, p.w, p.h, desiredX, desiredY,
                                         actualX, actualY, fbW, fbH);
                                }
                                // Always set p to the final actual (post-flip
                                // or unflipped) coords. The earlier else
                                // branch was resetting p.x/y to desiredX/Y
                                // when actual already matched — making the
                                // popup MAP at the unflipped wine coord
                                // (out of viewport).
                                p.x = actualX;
                                p.y = actualY;
                                /* Wine creates the popup BODY with
                                 * override_redirect=1 and configures it
                                 * to its real size, but never sends
                                 * XMapWindow on the body — only on the
                                 * 4 shadow decoration siblings. Wine
                                 * treats the body as IsViewable as soon
                                 * as its parent (wine virtual desktop)
                                 * is mapped. Without this, the body's
                                 * pixels never composite (compositor
                                 * checks p.mapped) even though wine
                                 * paints them via PutImage and treats
                                 * the popup as visible internally.
                                 * Mark as mapped on first non-trivial
                                 * configure; decorations also get this
                                 * but are filtered out by the
                                 * kMinPopupDim check in the compositor. */
                                bool justMapped = false;
                                if (!p.mapped && p.w > 1 && p.h > 1) {
                                    p.mapped = true;
                                    justMapped = true;
                                }
                                if (p.mapped) {
                                    dirty = true;
                                    dirtyCv.notify_one();
                                }
                                /* Send synthetic MapNotify to wine so its
                                 * x11drv internal state (data->current_state.
                                 * wm_state) flips to NormalState. Without
                                 * this, wine's input thread sees ButtonPress
                                 * on a window it thinks is still
                                 * WithdrawnState and drops the event before
                                 * dispatching to WindowProc — the popup
                                 * receives no clicks and JUCE's modal loop
                                 * times out / dismisses on the next outside
                                 * input. */
                                if (justMapped) {
                                    uint8_t mapNotify[32];
                                    memset(mapNotify, 0, 32);
                                    mapNotify[0] = 19;  /* MapNotify */
                                    write16(mapNotify, 2, (uint16_t)seq);
                                    write32(mapNotify, 4, cfgWid);
                                    write32(mapNotify, 8, cfgWid);
                                    mapNotify[12] = 1;  /* override_redirect */
                                    sendAllLocked(clientFd, mapNotify, 32);
                                }
                            }
                        }
                        /* DO NOT sync the flipped position back to windowManager_.
                         * Wine reads back positions via GetGeometry and will
                         * re-issue ConfigureWindow if our reported position
                         * differs from what wine asked for. That re-trips
                         * the smart-flip on the next ConfigureWindow and
                         * we end up oscillating (e.g. wine wants y=655,
                         * we flip to 615, wine sees 615 and asks for 655
                         * again, we flip to 575, ...). The visible side-
                         * effect is the popup body never settles (wine
                         * never gets to send MapWindow) and the sub-
                         * popup stays hidden.
                         *
                         * Side-effect: touch hit-test still uses windowManager
                         * coords (the unflipped position). Taps on the
                         * VISIBLE flipped popup will route to wine's
                         * intended position which is offscreen. Acceptable
                         * trade-off for now — sub-popups appear; if
                         * interactions break we'll add a reverse-map in
                         * injectTouch as a follow-up. */
                        (void)popupShifted;
                        (void)popupShiftedXFlag;
                        (void)popupShiftedYFlag;
                        (void)popupShiftedX;
                        (void)popupShiftedY;

                        if (isChildWin && (newW > 0 || newH > 0)) {
                            if (sizeChanged) {
                                auto pos = windowManager_.getPosition(cfgWid);
                                LOGI("X11 ConfigureWindow wid=0x%x size %dx%d pos=(%d,%d) parent=0x%x (vmask=0x%04x)",
                                     cfgWid, finalW, finalH, pos.x, pos.y, pos.parent, (unsigned)vmask);
                            }

                            /* Installer mode: track the largest mapped non-popup
                             * root-child window's bounds as the active crop rect.
                             * renderLoop renders this sub-rect of the framebuffer
                             * scaled to fill the viewport so the wizard appears
                             * large instead of tiny inside the 1024×768 framebuffer. */
                            /* Installer auto-crop attempt removed:
                             * cropW/cropH read as stale 1328x996 values
                             * even after explicit reset, suggesting the
                             * Impl isn't isolated between installer runs.
                             * Diagnosing the leak isn't worth the time
                             * vs the modest UX win (wizard fills viewport).
                             * Wizard still renders correctly inside the
                             * 1024×768 letterboxed framebuffer — just
                             * smaller than it could be. */

                            /* If this is the tracked plugin window and the size changed,
                             * resize the framebuffer — preserve existing pixels (no dark wipe). */
                            bool isTopLevel = false;
                            {
                                std::lock_guard<std::mutex> mapLock(windowMapMutex);
                                isTopLevel = !childWindows.empty() && cfgWid == childWindows[0];
                            }
                            if (sizeChanged && isTopLevel) {
                                std::lock_guard<std::mutex> fbLock(bufferMutex);
                                /* Installer mode (fbSizeFrozen) wants the
                                 * framebuffer locked at the size the caller
                                 * chose — typically a small virtual screen
                                 * like 640×480 so the wizard appears large
                                 * relative to the SurfaceView. Wine still
                                 * fires ConfigureWindow for its desktop and
                                 * wizard windows; the unconditional reset
                                 * here was clobbering pluginWidth back to
                                 * those sizes after setPluginSize ran. Track
                                 * slot for touch routing either way. */
                                if (!fbSizeFrozen) {
                                    pluginWidth  = finalW;
                                    pluginHeight = finalH;
                                    uint32_t bgX11 = 0xFF302020;
                                    /* resize() preserves existing pixels; only fills newly added pixels */
                                    framebuffer.resize((size_t)pluginWidth * pluginHeight, bgX11);
                                }
                                /* Register as plugin slot too so touch routing knows about it. */
                                bool already = false;
                                for (uint32_t w : pluginSlotWindows) if (w == cfgWid) { already = true; break; }
                                if (!already) pluginSlotWindows.push_back(cfgWid);
                            }
                            /* Claim editor ownership when ANY window grows to a
                             * sizable editor area — not just the top-level.
                             * JUCE plugins attach the editor as a child of a
                             * placeholder, and the disposable drawing client
                             * that floods PutImage often disconnects shortly
                             * after, leaving the main plugin thread without
                             * ownership. ConfigureWindow comes from the
                             * persistent main thread. */
                            if (sizeChanged && finalW >= 64 && finalH >= 64) {
                                int currentOwner = editorOwnerFd.load(std::memory_order_relaxed);
                                if (currentOwner < 0 || currentOwner == clientFd) {
                                    if (currentOwner < 0) {
                                        editorOwnerFd.store(clientFd, std::memory_order_release);
                                        LOGI("X11 editor owner claimed via ConfigureWindow: fd=%d wid=0x%x %dx%d",
                                             clientFd, cfgWid, finalW, finalH);
                                    }
                                }
                            }

                            /* ConfigureNotify is what wine's handle_state_change
                             * waits on to clear data->configure_serial. We
                             * must fire it whenever EITHER position OR size
                             * changes (and at least one of vmask's geometry
                             * bits was set), with the CURRENT request's
                             * serial so wine doesn't dismiss the event as
                             * "old". The original "size-change only" gate
                             * was an over-correction for a feedback loop
                             * that came from sending stale geometry; if we
                             * report the exact geometry wine just requested,
                             * wine's resize_event no-ops because the values
                             * match its pending state.
                             *
                             * Expose still gates on sizeChanged — sending
                             * spurious exposes wastes plugin paint cycles. */
                            bool geometryChanged = sizeChanged || posChanged;
                            if (geometryChanged) {
                                uint16_t evtSeq = (uint16_t)seq;
                                auto pos = windowManager_.getPosition(cfgWid);
                                uint8_t cfgNotify[32];
                                memset(cfgNotify, 0, 32);
                                cfgNotify[0] = 22;  /* ConfigureNotify */
                                write16(cfgNotify, 2, evtSeq);
                                write32(cfgNotify, 4, cfgWid);  /* event window */
                                write32(cfgNotify, 8, cfgWid);  /* window */
                                write32(cfgNotify, 12, 0);       /* above-sibling: None */
                                write16(cfgNotify, 16, (uint16_t)(int16_t)pos.x);
                                write16(cfgNotify, 18, (uint16_t)(int16_t)pos.y);
                                write16(cfgNotify, 20, (uint16_t)finalW);
                                write16(cfgNotify, 22, (uint16_t)finalH);
                                write16(cfgNotify, 24, 0);       /* border-width */
                                cfgNotify[26] = 0;               /* override-redirect */
                                sendAllLocked(clientFd, cfgNotify, 32);
                            }
                            if (sizeChanged) {
                                uint16_t evtSeq = (uint16_t)seq;
                                uint8_t expose[32];
                                memset(expose, 0, 32);
                                expose[0] = Expose;
                                write16(expose, 2, evtSeq);
                                write32(expose, 4, cfgWid);
                                write16(expose, 12, (uint16_t)finalW);
                                write16(expose, 14, (uint16_t)finalH);
                                sendAllLocked(clientFd, expose, 32);
                            }
                        } else {
                            /* No size in this configure (stack-mode-only or
                             * vmask=0). Still send ConfigureNotify if position
                             * changed via posChanged path so wine clears its
                             * configure_serial expectation. */
                            if (posChanged) {
                                uint16_t evtSeq = (uint16_t)seq;
                                auto sz = windowManager_.getSize(cfgWid);
                                auto pos = windowManager_.getPosition(cfgWid);
                                uint8_t cfgNotify[32];
                                memset(cfgNotify, 0, 32);
                                cfgNotify[0] = 22;
                                write16(cfgNotify, 2, evtSeq);
                                write32(cfgNotify, 4, cfgWid);
                                write32(cfgNotify, 8, cfgWid);
                                write32(cfgNotify, 12, 0);
                                write16(cfgNotify, 16, (uint16_t)(int16_t)pos.x);
                                write16(cfgNotify, 18, (uint16_t)(int16_t)pos.y);
                                write16(cfgNotify, 20, (uint16_t)sz.first);
                                write16(cfgNotify, 22, (uint16_t)sz.second);
                                write16(cfgNotify, 24, 0);
                                cfgNotify[26] = 0;
                                sendAllLocked(clientFd, cfgNotify, 32);
                            }
                            if (reqLogCount <= 20) LOGI("X11 ConfigureWindow wid=0x%x vmask=0x%04x (no size change)", cfgWid, (unsigned)vmask);
                        }

                        /* If a child window's position changed (XMoveWindow), send Expose
                         * for the top-level plugin window so the plugin redraws everything
                         * at the new positions.  Unlike a real X server, we can't relocate
                         * pixels in our single-framebuffer architecture — the client must
                         * redraw.  Multiple Expose events for the same window are coalesced
                         * by the client's event loop (XCheckTypedWindowEvent). */
                        if (posChanged && exposeTopWid && exposeTopW > 0 && exposeTopH > 0) {
                            uint16_t evtSeq = lastReplySeq_;
                            uint8_t expose[32];
                            memset(expose, 0, 32);
                            expose[0] = Expose;
                            write16(expose, 2, evtSeq);
                            write32(expose, 4, exposeTopWid);
                            write16(expose, 12, (uint16_t)exposeTopW);
                            write16(expose, 14, (uint16_t)exposeTopH);
                            sendAllLocked(clientFd, expose, 32);
                        }
                        break;
                    }
                    case kGLXMajorOpcode: {
                        /* GLX extension. Mesa's software loader (drisw + llvmpipe) does ALL
                         * GL rendering client-side and presents via core XPutImage, so the
                         * server only needs correct GLX config/string/context bookkeeping —
                         * no server-side GL. (Hardware DRI3/DRI2 + Present is a separate path.)
                         * Sub-opcode is in buf[1]; the (small) request body is at buf+4.
                         *
                         * Authoritative GLX sub-opcodes (glxproto.h) — rep = expects a reply:
                         *   1 Render            2 RenderLarge       3 CreateContext
                         *   4 DestroyContext    5 MakeCurrent(rep)  6 IsDirect(rep)
                         *   7 QueryVersion(rep) 8 WaitGL  9 WaitX   10 CopyContext
                         *   11 SwapBuffers      12 UseXFont         13 CreateGLXPixmap
                         *   14 GetVisualConfigs(rep)  15 DestroyGLXPixmap  16 VendorPrivate
                         *   17 VendorPrivateWithReply(rep)  18 QueryExtensionsString(rep)
                         *   19 QueryServerString(rep)  20 ClientInfo  21 GetFBConfigs(rep)
                         *   22 CreatePixmap  23 DestroyPixmap  24 CreateNewContext
                         *   25 QueryContext(rep)  26 MakeContextCurrent(rep)
                         *   27 CreatePbuffer 28 DestroyPbuffer  29 GetDrawableAttributes(rep)
                         *   30 ChangeDrawableAttributes 31 CreateWindow 32 DestroyWindow
                         *   33 SetClientInfoARB 34 CreateContextAttribsARB 35 SetClientInfo2ARB
                         */
                        uint8_t glxMinor = buf[1];
                        LOGI("X11 handle GLX sub-opcode=%u length=%u", (unsigned)glxMinor, (unsigned)length);

                        /* String reply (QueryServerString / QueryExtensionsString): byte
                         * length incl null at offset 12, length=pad(n)/4, string at 32. */
                        auto sendGlxString = [&](const char* s) {
                            uint32_t n = (uint32_t)strlen(s) + 1;
                            size_t padded = ((size_t)n + 3) & ~size_t(3);
                            std::vector<uint8_t> reply(32 + padded, 0);
                            reply[0] = 1;
                            write16(reply.data(), 2, seq);
                            write32(reply.data(), 4, (uint32_t)(padded / 4));
                            write32(reply.data(), 12, n);
                            memcpy(reply.data() + 32, s, n);
                            sendReply(reply.data(), reply.size(), seq);
                        };
                        /* (tag,value)-pair reply (QueryContext / GetDrawableAttributes):
                         * pair count at offset 8, then count CARD32 (tag,value) pairs. */
                        auto sendGlxPairs = [&](const std::vector<std::pair<uint32_t,uint32_t>>& a) {
                            size_t dataBytes = a.size() * 2 * 4;
                            std::vector<uint8_t> reply(32 + dataBytes, 0);
                            reply[0] = 1;
                            write16(reply.data(), 2, seq);
                            write32(reply.data(), 4, (uint32_t)(dataBytes / 4));
                            write32(reply.data(), 8, (uint32_t)a.size());
                            size_t off = 32;
                            for (auto& kv : a) { write32(reply.data(), off, kv.first); off += 4;
                                                 write32(reply.data(), off, kv.second); off += 4; }
                            sendReply(reply.data(), reply.size(), seq);
                        };
                        static const char* kGlxExt =
                            "GLX_ARB_create_context GLX_ARB_create_context_profile "
                            "GLX_EXT_create_context_es2_profile GLX_ARB_get_proc_address "
                            "GLX_EXT_visual_info GLX_EXT_visual_rating GLX_ARB_multisample";

                        switch (glxMinor) {
                            case 7: { /* glXQueryVersion -> 1.4 */
                                uint8_t reply[32]; memset(reply, 0, 32);
                                reply[0] = 1; write16(reply, 2, seq);
                                write32(reply, 8, 1); write32(reply, 12, 4);
                                sendReply(reply, 32, seq);
                                break;
                            }
                            case 14: { /* glXGetVisualConfigs: 18 positional + tagged pairs */
                                std::vector<uint32_t> p;
                                const uint32_t pos[18] = {
                                    kDefaultVisualId, 4 /*TrueColor*/, 1 /*rgba*/,
                                    8,8,8,8, 0,0,0,0, 1 /*dbl*/, 0 /*stereo*/,
                                    32 /*bufsize*/, 24 /*depth*/, 8 /*stencil*/, 0 /*aux*/, 0 /*level*/ };
                                for (uint32_t v : pos) p.push_back(v);
                                auto tg = [&](uint32_t k, uint32_t v){ p.push_back(k); p.push_back(v); };
                                tg(0x20, 0x8000);   /* CAVEAT = NONE */
                                tg(0x22, 0x8002);   /* X_VISUAL_TYPE = TRUE_COLOR */
                                tg(0x8011, 0x1);    /* RENDER_TYPE = RGBA_BIT */
                                tg(0x8010, 0x7);    /* DRAWABLE_TYPE = win|pix|pbuf */
                                tg(0x8012, 1);      /* X_RENDERABLE */
                                tg(0x8013, 1);      /* FBCONFIG_ID */
                                size_t dataBytes = p.size() * 4;
                                std::vector<uint8_t> reply(32 + dataBytes, 0);
                                reply[0] = 1; write16(reply.data(), 2, seq);
                                write32(reply.data(), 4, (uint32_t)(dataBytes / 4));
                                write32(reply.data(), 8, 1);                    /* numVisuals */
                                write32(reply.data(), 12, (uint32_t)p.size());  /* props per visual */
                                for (size_t i = 0; i < p.size(); i++) write32(reply.data(), 32 + i*4, p[i]);
                                sendReply(reply.data(), reply.size(), seq);
                                break;
                            }
                            case 21: { /* glXGetFBConfigs: all (tag,value) pairs */
                                const std::vector<std::pair<uint32_t,uint32_t>> a = {
                                    {0x8013, 1},                 /* FBCONFIG_ID */
                                    {0x800B, kDefaultVisualId},  /* VISUAL_ID (maps config -> X visual) */
                                    {0x22, 0x8002},              /* X_VISUAL_TYPE = TRUE_COLOR */
                                    {0x8011, 0x1},               /* RENDER_TYPE = RGBA_BIT */
                                    {0x8010, 0x7},               /* DRAWABLE_TYPE = win|pix|pbuf */
                                    {0x8012, 1},                 /* X_RENDERABLE */
                                    {0x20, 0x8000},              /* CONFIG_CAVEAT = NONE */
                                    {0x2, 32},                   /* BUFFER_SIZE */
                                    {0x3, 0},                    /* LEVEL */
                                    {0x5, 1},                    /* DOUBLEBUFFER */
                                    {0x6, 0},                    /* STEREO */
                                    {0x7, 0},                    /* AUX_BUFFERS */
                                    {0x8, 8}, {0x9, 8}, {0xA, 8}, {0xB, 8},   /* R G B A */
                                    {0xC, 24},                   /* DEPTH */
                                    {0xD, 8},                    /* STENCIL */
                                    {0xE, 0}, {0xF, 0}, {0x10, 0}, {0x11, 0}, /* accum R G B A */
                                    {0x23, 0x8000},              /* TRANSPARENT_TYPE = NONE */
                                    {0x186A0, 0},                /* SAMPLE_BUFFERS */
                                    {0x186A1, 0},                /* SAMPLES */
                                };
                                size_t dataBytes = a.size() * 2 * 4;
                                std::vector<uint8_t> reply(32 + dataBytes, 0);
                                reply[0] = 1; write16(reply.data(), 2, seq);
                                write32(reply.data(), 4, (uint32_t)(dataBytes / 4));
                                write32(reply.data(), 8, 1);                    /* numFBConfigs */
                                write32(reply.data(), 12, (uint32_t)a.size());  /* attribute pairs per config */
                                size_t off = 32;
                                for (auto& kv : a) { write32(reply.data(), off, kv.first); off += 4;
                                                     write32(reply.data(), off, kv.second); off += 4; }
                                sendReply(reply.data(), reply.size(), seq);
                                break;
                            }
                            case 18:   /* glXQueryExtensionsString */
                                sendGlxString(kGlxExt);
                                break;
                            case 19: { /* glXQueryServerString: body = screen@4, name@8 */
                                uint32_t name = read32(buf, 8);
                                const char* s = (name == 1) ? "GuitarRackCraft"
                                              : (name == 2) ? "1.4"
                                              : (name == 3) ? kGlxExt : "";
                                sendGlxString(s);
                                break;
                            }
                            case 5:    /* glXMakeCurrent -> context tag */
                            case 26: { /* glXMakeContextCurrent -> context tag */
                                uint8_t reply[32]; memset(reply, 0, 32);
                                reply[0] = 1; write16(reply, 2, seq);
                                write32(reply, 8, 1);  /* non-zero context tag = success */
                                sendReply(reply, 32, seq);
                                break;
                            }
                            case 6: {  /* glXIsDirect */
                                uint8_t reply[32]; memset(reply, 0, 32);
                                reply[0] = 1; write16(reply, 2, seq);
                                reply[8] = 0;  /* server sees the context as indirect */
                                sendReply(reply, 32, seq);
                                break;
                            }
                            case 25:   /* glXQueryContext */
                                sendGlxPairs({ {0x8013, 1}, {0x8011, 0x8014 /*GLX_RGBA_TYPE*/}, {0x800C, 0} });
                                break;
                            case 29: { /* glXGetDrawableAttributes -> size + config id */
                                int w = pluginWidth  > 0 ? pluginWidth  : (width  > 0 ? width  : 1280);
                                int h = pluginHeight > 0 ? pluginHeight : (height > 0 ? height : 720);
                                sendGlxPairs({ {0x801D, (uint32_t)w} /*WIDTH*/, {0x801E, (uint32_t)h} /*HEIGHT*/,
                                               {0x8013, 1} /*FBCONFIG_ID*/, {0x801B, 1} /*PRESERVED_CONTENTS*/ });
                                break;
                            }
                            case 17: { /* glXVendorPrivateWithReply -> generic empty reply */
                                uint8_t reply[32]; memset(reply, 0, 32);
                                reply[0] = 1; write16(reply, 2, seq);
                                sendReply(reply, 32, seq);
                                break;
                            }
                            /* Void GLX requests (no reply). */
                            case 1: case 2: case 3: case 4: case 8: case 9: case 10:
                            case 11: case 12: case 13: case 15: case 16: case 20:
                            case 22: case 23: case 24: case 27: case 28: case 30:
                            case 31: case 32: case 33: case 34: case 35:
                                break;
                            default: {
                                /* Unknown sub-opcode: send a generic reply (matches prior
                                 * behavior; reply-bearing requests would otherwise hang). */
                                LOGI("X11 GLX unhandled sub-opcode=%u (generic reply)", (unsigned)glxMinor);
                                uint8_t reply[32]; memset(reply, 0, 32);
                                reply[0] = 1; write16(reply, 2, seq);
                                sendReply(reply, 32, seq);
                                break;
                            }
                        }
                        break;
                    }
                    case ChangeProperty: {
                        /* ChangeProperty is a void request — no reply. But
                         * wine's user32 listens for PropertyNotify events
                         * to confirm property writes. Java X server fires
                         * a PropertyNotify after each ChangeProperty, and
                         * wine relies on this during CreateWindowExA's
                         * window-property init. Without these events,
                         * wine's user32 keyboard/window setup deadlocks
                         * inside set_window_long's wineserver IPC.
                         *
                         * Request body (already in buf):
                         *   buf[1]:    mode (Replace=0/Prepend/Append)
                         *   buf[4-7]:  window
                         *   buf[8-11]: property (atom)
                         *   buf[12-15]: type (atom)
                         *   buf[16]:   format
                         *
                         * PropertyNotify event format (32 bytes):
                         *   byte 0:    code = 28
                         *   byte 1:    unused
                         *   bytes 2-3: sequence
                         *   bytes 4-7: window
                         *   bytes 8-11: atom (property)
                         *   bytes 12-15: time
                         *   byte 16:   state (NewValue=0, Deleted=1)
                         *   bytes 17-31: pad */
                        uint32_t cpWid = read32(buf, 4);
                        uint32_t cpAtom = read32(buf, 8);
                        /* Store the written value so GetProperty can echo it
                         * back exactly (wine's 11.9 state machine reads back
                         * what it writes and rejects mismatches). Header:
                         * type@12, format@16, value_length(items)@20, data@24. */
                        {
                            uint8_t cpFmt = buf[16];
                            uint32_t cpType = read32(buf, 12);
                            uint32_t cpItems = read32(buf, 20);
                            uint32_t cpUnit = (cpFmt == 32) ? 4 : (cpFmt == 16 ? 2 : 1);
                            size_t cpBytes = (size_t)cpItems * cpUnit;
                            if (cpFmt && (size_t)24 + cpBytes <= (size_t)length * 4) {
                                uint64_t pk = ((uint64_t)cpWid << 32) | cpAtom;
                                std::lock_guard<std::mutex> lk(propStoreMutex);
                                auto& pv = propStore_[pk];
                                if (buf[1] != 0 /*Prepend/Append*/ && pv.format == cpFmt && pv.type == cpType) {
                                    if (buf[1] == 1) pv.data.insert(pv.data.begin(), buf + 24, buf + 24 + cpBytes);
                                    else pv.data.insert(pv.data.end(), buf + 24, buf + 24 + cpBytes);
                                } else { /* Replace (or incompatible type/format) */
                                    pv.type = cpType; pv.format = cpFmt;
                                    pv.data.assign(buf + 24, buf + 24 + cpBytes);
                                }
                            }
                        }
                        /* Modern wine doesn't call XMapWindow for top-level
                         * windows — it sets the WM_STATE atom to
                         * NormalState (1) and expects a window manager to
                         * observe that and call XMapWindow itself. We have
                         * no WM, so dialog windows like X50II's login form
                         * stay unmapped from our hit-test perspective even
                         * though wine considers them visible. Same for
                         * WithdrawnState (0) — wine uses that as "hide".
                         * Treat WM_STATE writes as implicit map/unmap. */
                        std::string atomName = atoms_.getName(cpAtom);
                        if (atomName == "WM_STATE" && length >= 7) {
                            /* Property payload starts at byte 24 after the
                             * 24-byte ChangeProperty header (with explicit
                             * length / type / format fields). The first
                             * word of the WM_STATE payload is the state. */
                            uint32_t state = read32(buf, 24);
                            std::lock_guard<std::mutex> mapLock(windowMapMutex);
                            if (state == 1 /*NormalState*/) {
                                windowManager_.mapWindow(cpWid);
                                LOGI("X11 WM_STATE NormalState -> implicit map wid=0x%x", cpWid);
                            } else if (state == 0 /*WithdrawnState*/) {
                                windowManager_.unmapWindow(cpWid);
                                LOGI("X11 WM_STATE WithdrawnState -> implicit unmap wid=0x%x", cpWid);
                            }
                        }
                        /* NOTE: a value-dedup of redundant hint PropertyNotify
                         * events used to live here (band-aid for TH-U's relayout
                         * storm). It is REMOVED: wine 11.9's serial-strict state
                         * machine needs a correctly-serialed PropertyNotify for
                         * EVERY ChangeProperty to confirm its expect_serial —
                         * skipping a re-write's notify leaves that serial pending
                         * ("old serial" drops), and for the readiness-gating atoms
                         * (_MOTIF_WM_HINTS/_NET_WM_STATE) that stalls the window.
                         * The real cure for the storm is the property store above
                         * (correct read-back → state converges → no re-request
                         * amplification), which made the storm disappear. */
                        uint8_t evt[32];
                        memset(evt, 0, 32);
                        evt[0] = 28;  /* PropertyNotify */
                        write16(evt, 2, seq);
                        write32(evt, 4, cpWid);
                        write32(evt, 8, cpAtom);
                        /* time: just use seq scaled — wine doesn't care
                         * about wall time, just monotonic ordering. */
                        write32(evt, 12, (uint32_t)(seq * 4 + 0x300));
                        evt[16] = 0;  /* state = NewValue */
                        /* Send through the same socket. sendReply has the
                         * right protocol for this even though it's an
                         * event — we just need bytes on the wire. */
                        sendReply(evt, 32, seq);
                        break;
                    }
                    case 42: { /* SetInputFocus */
                        /* req body: byte 1 = revert_to, bytes 4-7 = focus
                         * window, bytes 8-11 = time. We track the focus
                         * target purely for diagnosis — keyboard event
                         * routing in injectKey uses grabWindow (the
                         * last-clicked widget). */
                        uint32_t focusWid = (length >= 3) ? read32(buf, 4) : 0;
                        uint32_t prevWid = focusedWindowId.exchange(focusWid, std::memory_order_acq_rel);
                        static thread_local int siDbg = 0;
                        if (++siDbg <= 30 || siDbg % 20 == 0) {
                            LOGI("X11 SetInputFocus display=%d wid=0x%x (prev=0x%x)",
                                 displayNumber_, focusWid, prevWid);
                        }
                        /* X11 spec: SetInputFocus generates FocusOut on the
                         * previously focused window and FocusIn on the new
                         * one. Wine's winex11.drv tracks last_focus via these
                         * events; without them, even after wine calls
                         * SetInputFocus itself it never marks the window as
                         * having keyboard focus and routes incoming KeyPress
                         * events into the void. EDIT controls then silently
                         * drop typed characters. */
                        auto sendFocusEvent = [&](uint8_t type, uint32_t wid) {
                            int fd = fdForWindow(wid);
                            uint16_t evtSeq = lastReplySeq_;
                            if (fd != clientFd) {
                                std::lock_guard<std::mutex> lk(fdSeqMutex);
                                auto it = fdLastSeq.find(fd);
                                if (it != fdLastSeq.end()) evtSeq = it->second;
                            }
                            uint8_t evt[32];
                            memset(evt, 0, 32);
                            evt[0] = type;
                            evt[1] = 3;  /* detail = NotifyNonlinear */
                            write16(evt, 2, evtSeq);
                            write32(evt, 4, wid);
                            evt[8] = 0;  /* mode = NotifyNormal */
                            sendAllLocked(fd, evt, 32);
                        };
                        /* 0x4 = suppress the FocusOut/FocusIn echo for wine's own
                         * XSetInputFocus (let wine's focus stand without our echo). */
                        if (!(focusModeMask_.load(std::memory_order_relaxed) & 0x4u)) {
                            if (prevWid && prevWid != focusWid) sendFocusEvent(10 /*FocusOut*/, prevWid);
                            if (focusWid && focusWid != prevWid) sendFocusEvent(9 /*FocusIn*/, focusWid);
                        }
                        break;
                    }
                    case DeleteProperty:
                        /* Drop the stored value so a later GetProperty read-back
                         * returns empty, AND emit PropertyNotify(state=Deleted) —
                         * the X spec sends it when the property existed, and wine
                         * 11.9's write/delete-then-confirm pattern can wait on it
                         * (same class as the GetProperty-returns-empty bug). */
                        if (length >= 3) {
                            uint32_t dpWid = read32(buf, 4), dpAtom = read32(buf, 8);
                            bool existed = false;
                            {
                                std::lock_guard<std::mutex> lk(propStoreMutex);
                                existed = propStore_.erase(((uint64_t)dpWid << 32) | dpAtom) > 0;
                            }
                            if (existed) {
                                uint8_t evt[32];
                                memset(evt, 0, 32);
                                evt[0] = 28;  /* PropertyNotify */
                                write16(evt, 2, seq);
                                write32(evt, 4, dpWid);
                                write32(evt, 8, dpAtom);
                                write32(evt, 12, (uint32_t)(seq * 4 + 0x300));
                                evt[16] = 1;  /* state = Deleted */
                                sendReply(evt, 32, seq);
                            }
                        }
                        break;
                    case 22: /* SetSelectionOwner (void) */
                    case 24: /* ConvertSelection (void) */
                    case 51: /* SetFontPath */
                        break;
                    case 55: /* CreateGC */
                    {
                        /* Request: opcode(1), pad(1), length(2), cid(4),
                         * drawable(4), value-mask(4), value-list[]
                         * value-list is ordered by mask bits, lowest first.
                         * Bit 2 = GCForeground (CARD32, padded to 4 bytes). */
                        if (length >= 4) {
                            uint32_t cid = read32(buf, 4);
                            uint32_t mask = read32(buf, 12);
                            int off = 16;
                            for (int bit = 0; bit < 23; ++bit) {
                                if (mask & (1u << bit)) {
                                    if (bit == 2 /*GCForeground*/ && off + 4 <= (int)length * 4) {
                                        uint32_t fg = read32(buf, off);
                                        std::lock_guard<std::mutex> lk(gcMutex);
                                        gcForeground[cid] = fg;
                                    }
                                    off += 4;
                                }
                            }
                        }
                        break;
                    }
                    case 56: /* ChangeGC */
                    {
                        /* Request: opcode(1), pad(1), length(2), gc(4),
                         * value-mask(4), value-list[] */
                        if (length >= 3) {
                            uint32_t gc = read32(buf, 4);
                            uint32_t mask = read32(buf, 8);
                            int off = 12;
                            for (int bit = 0; bit < 23; ++bit) {
                                if (mask & (1u << bit)) {
                                    if (bit == 2 /*GCForeground*/ && off + 4 <= (int)length * 4) {
                                        uint32_t fg = read32(buf, off);
                                        std::lock_guard<std::mutex> lk(gcMutex);
                                        gcForeground[gc] = fg;
                                    }
                                    off += 4;
                                }
                            }
                        }
                        break;
                    }
                    case 60: /* FreeGC */
                    {
                        if (length >= 2) {
                            uint32_t gc = read32(buf, 4);
                            std::lock_guard<std::mutex> lk(gcMutex);
                            gcForeground.erase(gc);
                        }
                        break;
                    }
                    case 57: /* CopyGC */
                    case 58: /* SetDashes */
                    case 59: /* SetClipRectangles */
                    case 61: /* ClearArea — actually has reply if exposures=1; treat as void for simplicity */
                    case 63: /* CopyPlane */
                    case 64: /* PolyPoint */
                    case 65: /* PolyLine */
                    case 66: /* PolySegment */
                    case 67: /* PolyRectangle */
                    case 68: /* PolyArc */
                    case 69: /* FillPoly */
                    case 71: /* PolyFillArc */
                    case 78: /* CreateColormap */
                    case 79: /* FreeColormap */
                    case 100: /* ChangeKeyboardMapping */
                        /* first-seen already logged at opcode entry above */
                        break;
                    case 101: { /* GetKeyboardMapping — return Java's exact
                                   reply bytes. Wine's libX11 keymap init
                                   needs real keysyms (not NoSymbol) for
                                   common keys or it falls into a probe
                                   loop / deadlock during CreateWindowExA.
                                   Captured from au.com.darkside.xserver
                                   while wine was successfully rendering
                                   the WagnerSharp editor.

                                   kpk=3, 157 keycodes' worth of keysyms
                                   (1884-byte payload), reply_length=471.
                                   Number-row letters, arrow keys, and a
                                   few extras are populated; rest are
                                   NoSymbol. */
                        // Java starts the keysym data at byte 0 of the
                        // payload (which is byte 32 of the reply) with
                        // keycode 8 = '0'/')' — confirmed from capture.
                        // Each keycode gets 3 keysyms (kpk=3), 4 bytes each
                        // = 12 bytes per keycode.
                        static const uint8_t kJavaKeymapPayload[1884] = {
                          /* keycode 8 = '0' / ')' / NoSymbol */
                          0x30,0,0,0, 0x29,0,0,0, 0,0,0,0,
                          /* keycode 9 = '1' / '!' */
                          0x31,0,0,0, 0x21,0,0,0, 0,0,0,0,
                          /* keycode 10 = '2' / '@' */
                          0x32,0,0,0, 0x40,0,0,0, 0,0,0,0,
                          /* keycode 11 = '3' / '#' */
                          0x33,0,0,0, 0x23,0,0,0, 0,0,0,0,
                          /* keycode 12 = '4' / '$' */
                          0x34,0,0,0, 0x24,0,0,0, 0,0,0,0,
                          /* keycode 13 = '5' / '%' */
                          0x35,0,0,0, 0x25,0,0,0, 0,0,0,0,
                          /* keycode 14 = '6' / '^' */
                          0x36,0,0,0, 0x5e,0,0,0, 0,0,0,0,
                          /* keycode 15 = '7' / '&' */
                          0x37,0,0,0, 0x26,0,0,0, 0,0,0,0,
                          /* keycode 16 = '8' / '*' */
                          0x38,0,0,0, 0x2a,0,0,0, 0,0,0,0,
                          /* keycode 17 = '9' / '(' */
                          0x39,0,0,0, 0x28,0,0,0, 0,0,0,0,
                          /* keycode 18 = '*' / '*' */
                          0x2a,0,0,0, 0x2a,0,0,0, 0,0,0,0,
                          /* keycode 19 = '#' / '#' */
                          0x23,0,0,0, 0x23,0,0,0, 0,0,0,0,
                          /* keycode 20 = Up arrow (XK_Up = 0xff52) */
                          0x52,0xff,0,0, 0,0,0,0, 0,0,0,0,
                          /* keycode 21 = Down arrow (XK_Down = 0xff54) */
                          0x54,0xff,0,0, 0,0,0,0, 0,0,0,0,
                          /* keycode 22 = Left arrow (XK_Left = 0xff51) */
                          0x51,0xff,0,0, 0,0,0,0, 0,0,0,0,
                          /* keycode 23 = Right arrow (XK_Right = 0xff53) */
                          0x53,0xff,0,0, 0,0,0,0, 0,0,0,0,
                          /* keycodes 24..49 = letters a..z (lowercase, uppercase,
                             NoSymbol). injectKey looks up these slots when an
                             ASCII char comes in. Order is alphabetical, not
                             QWERTY-mechanical — the keycode-to-keysym mapping
                             is what wine sees; we never expose the layout. */
                          0x61,0,0,0, 0x41,0,0,0, 0,0,0,0,  /* 24 a/A */
                          0x62,0,0,0, 0x42,0,0,0, 0,0,0,0,  /* 25 b/B */
                          0x63,0,0,0, 0x43,0,0,0, 0,0,0,0,  /* 26 c/C */
                          0x64,0,0,0, 0x44,0,0,0, 0,0,0,0,  /* 27 d/D */
                          0x65,0,0,0, 0x45,0,0,0, 0,0,0,0,  /* 28 e/E */
                          0x66,0,0,0, 0x46,0,0,0, 0,0,0,0,  /* 29 f/F */
                          0x67,0,0,0, 0x47,0,0,0, 0,0,0,0,  /* 30 g/G */
                          0x68,0,0,0, 0x48,0,0,0, 0,0,0,0,  /* 31 h/H */
                          0x69,0,0,0, 0x49,0,0,0, 0,0,0,0,  /* 32 i/I */
                          0x6a,0,0,0, 0x4a,0,0,0, 0,0,0,0,  /* 33 j/J */
                          0x6b,0,0,0, 0x4b,0,0,0, 0,0,0,0,  /* 34 k/K */
                          0x6c,0,0,0, 0x4c,0,0,0, 0,0,0,0,  /* 35 l/L */
                          0x6d,0,0,0, 0x4d,0,0,0, 0,0,0,0,  /* 36 m/M */
                          0x6e,0,0,0, 0x4e,0,0,0, 0,0,0,0,  /* 37 n/N */
                          0x6f,0,0,0, 0x4f,0,0,0, 0,0,0,0,  /* 38 o/O */
                          0x70,0,0,0, 0x50,0,0,0, 0,0,0,0,  /* 39 p/P */
                          0x71,0,0,0, 0x51,0,0,0, 0,0,0,0,  /* 40 q/Q */
                          0x72,0,0,0, 0x52,0,0,0, 0,0,0,0,  /* 41 r/R */
                          0x73,0,0,0, 0x53,0,0,0, 0,0,0,0,  /* 42 s/S */
                          0x74,0,0,0, 0x54,0,0,0, 0,0,0,0,  /* 43 t/T */
                          0x75,0,0,0, 0x55,0,0,0, 0,0,0,0,  /* 44 u/U */
                          0x76,0,0,0, 0x56,0,0,0, 0,0,0,0,  /* 45 v/V */
                          0x77,0,0,0, 0x57,0,0,0, 0,0,0,0,  /* 46 w/W */
                          0x78,0,0,0, 0x58,0,0,0, 0,0,0,0,  /* 47 x/X */
                          0x79,0,0,0, 0x59,0,0,0, 0,0,0,0,  /* 48 y/Y */
                          0x7a,0,0,0, 0x5a,0,0,0, 0,0,0,0,  /* 49 z/Z */
                          /* keycodes 50..63 = symbols + control keys.
                             Modifier bit-positions (state field) are X11's
                             standard: Shift=1<<0, Control=1<<2, Mod1=1<<3. */
                          0x20,0,0,0, 0x20,0,0,0, 0,0,0,0,  /* 50 space */
                          0x09,0xff,0,0, 0,0,0,0, 0,0,0,0,  /* 51 XK_Tab */
                          0x0d,0xff,0,0, 0,0,0,0, 0,0,0,0,  /* 52 XK_Return */
                          0x08,0xff,0,0, 0,0,0,0, 0,0,0,0,  /* 53 XK_BackSpace */
                          0xff,0xff,0,0, 0,0,0,0, 0,0,0,0,  /* 54 XK_Delete (0xffff) */
                          0x2e,0,0,0, 0x3e,0,0,0, 0,0,0,0,  /* 55 . / >       */
                          0x2c,0,0,0, 0x3c,0,0,0, 0,0,0,0,  /* 56 , / <       */
                          0x2f,0,0,0, 0x3f,0,0,0, 0,0,0,0,  /* 57 / / ?       */
                          0x40,0,0,0, 0x40,0,0,0, 0,0,0,0,  /* 58 @ (no shift)*/
                          0x2d,0,0,0, 0x5f,0,0,0, 0,0,0,0,  /* 59 - / _       */
                          0x3a,0,0,0, 0x3a,0,0,0, 0,0,0,0,  /* 60 : (no shift)*/
                          0x3b,0,0,0, 0x3a,0,0,0, 0,0,0,0,  /* 61 ; / :       */
                          0x27,0,0,0, 0x22,0,0,0, 0,0,0,0,  /* 62 ' / "       */
                          0x1b,0xff,0,0, 0,0,0,0, 0,0,0,0,  /* 63 XK_Escape   */
                          /* Modifier keys at their conventional keycodes so
                             wine's keymap probe sees them. Shift_L = 0xffe1.
                             We don't expect to inject a "Shift press" event
                             directly — modifier state is conveyed via the
                             state field of the KeyPress event — but wine
                             needs to KNOW the layout has a shift to
                             interpret the state bit correctly. */
                          0,0,0,0, 0,0,0,0, 0,0,0,0,        /* 64 unused      */
                          0xe1,0xff,0,0, 0,0,0,0, 0,0,0,0,  /* 65 XK_Shift_L  */
                          0xe2,0xff,0,0, 0,0,0,0, 0,0,0,0,  /* 66 XK_Shift_R  */
                          0xe3,0xff,0,0, 0,0,0,0, 0,0,0,0,  /* 67 XK_Control_L*/
                          /* rest of keycodes (#68..#164) — all NoSymbol;
                             ~97 keycodes × 12 bytes = ~1164 bytes of zeros
                             follow (zero-initialized via trailing brace). */
                        };
                        const uint8_t kpk = 3;
                        const uint32_t replyLengthWords = 471;  /* Java's value */
                        const size_t dataBytes = (size_t)replyLengthWords * 4;
                        std::vector<uint8_t> reply(32 + dataBytes, 0);
                        reply[0] = 1;
                        reply[1] = kpk;
                        write16(reply.data(), 2, seq);
                        write32(reply.data(), 4, replyLengthWords);  /* 471 words = Java */
                        /* Copy known keysyms (covers ~22 keycodes); the rest
                         * stay NoSymbol (zero-initialized vector). */
                        const size_t copyBytes = sizeof(kJavaKeymapPayload) < dataBytes
                            ? sizeof(kJavaKeymapPayload) : dataBytes;
                        memcpy(reply.data() + 32, kJavaKeymapPayload, copyBytes);
                        /* Dump first 6 keycodes (72 bytes) of the keysym data
                         * so we can verify wine reads what we send. */
                        {
                            uint8_t firstKc = (length >= 2) ? buf[4] : 0;
                            uint8_t count = (length >= 2) ? buf[5] : 0;
                            char hex[3 * 80 + 1];
                            size_t off = 0;
                            for (size_t i = 0; i < 80 && i < copyBytes; ++i)
                                off += (size_t)snprintf(hex + off, sizeof(hex) - off,
                                                        "%02x ", reply.data()[32 + i]);
                            LOGI("X11 GetKeyboardMapping req first_kc=%u count=%u "
                                 "reply kpk=%u length_words=%u; payload[0..80]: %s",
                                 firstKc, count, (unsigned)kpk, (unsigned)replyLengthWords, hex);
                        }
                        sendReply(reply.data(), reply.size(), seq);
                        break;
                    }
                    default: {
                        /* Unknown opcode — do NOT send a reply: sending an unexpected reply causes
                           protocol desync which crashes libX11. Log once per opcode. */
                        thread_local bool warnedUnhandled[256] = {};
                        if (!warnedUnhandled[opcode]) {
                            warnedUnhandled[opcode] = true;
                            LOGI("X11 UNHANDLED opcode=%u %s (ignored, no reply)", (unsigned)opcode, x11OpcodeName(opcode));
                        }
                        break;
                    }
                }

                /* Per-request timing */
                {
                    auto reqEnd = std::chrono::steady_clock::now();
                    auto thisReqUs = std::chrono::duration_cast<std::chrono::microseconds>(reqEnd - reqStart).count();
                    static thread_local long long totalSwitchUs = 0;
                    static thread_local int switchCount = 0;
                    static thread_local long long slowestUs = 0;
                    static thread_local uint8_t slowestOp = 0;
                    static thread_local auto lastSwitchLog = std::chrono::steady_clock::now();
                    totalSwitchUs += thisReqUs;
                    switchCount++;
                    if (thisReqUs > slowestUs) { slowestUs = thisReqUs; slowestOp = opcode; }
                    if (thisReqUs > 5000) {
                        LOGI("X11Stats: SLOW req opcode=%u %s took %lldus", (unsigned)opcode, x11OpcodeName(opcode), (long long)thisReqUs);
                    }
                    if (std::chrono::duration<double>(reqEnd - lastSwitchLog).count() >= 2.0) {
                        LOGI("X11Stats: OtherReqs %d calls in 2s, total=%lldms avg=%lldus slowest=%lldus op=%u(%s)",
                             switchCount, totalSwitchUs / 1000, switchCount > 0 ? totalSwitchUs / switchCount : 0,
                             slowestUs, (unsigned)slowestOp, x11OpcodeName(slowestOp));
                        totalSwitchUs = 0; switchCount = 0; slowestUs = 0; slowestOp = 0; lastSwitchLog = reqEnd;
                    }
                }
                /* After processing each request, drain any queued touch events. */
                drainTouchQueue();
            }
            LOGI("X11Close: X11 request loop ended tid=%ld (recv<=0 or !running), closing client fd=%d", getTid(), clientFd);
            if (clientFd >= 0) {
                int fdToClose = clientFd;
                /* If this was the editor owner, drop the claim so the next
                 * sizable PutImage can re-elect a new owner. Otherwise
                 * touch events keep getting routed to a dead fd. */
                int expectedOwner = fdToClose;
                editorOwnerFd.compare_exchange_strong(expectedOwner, -1);
                close(fdToClose);
                clientFd = -1;
                std::lock_guard<std::mutex> lk(activeClientsMutex);
                activeClientFds.erase(
                    std::remove(activeClientFds.begin(), activeClientFds.end(), fdToClose),
                    activeClientFds.end());
            }
            }).detach();   // end of per-connection std::thread lambda
        }                  // end of accept loop
        if (serverFd >= 0) {
            close(serverFd);
            serverFd = -1;
        }
    }

    void pluginUILoop() {
        LOGI("X11Debug: pluginUI thread STARTED display=%d tid=%ld", displayNumber_, getTid());
        int loopCount = 0;
        while (pluginUIRunning) {
            auto loopStart = std::chrono::steady_clock::now();
            loopCount++;

            // Process any queued tasks (instantiate, cleanup, port_event, etc.)
            int taskCount = 0;
            {
                std::unique_lock<std::mutex> lock(pluginUITaskMutex);
                while (!pluginUITasks.empty()) {
                    auto task = std::move(pluginUITasks.front());
                    pluginUITasks.pop();
                    lock.unlock();
                    auto taskStart = std::chrono::steady_clock::now();
                    task();
                    auto taskUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - taskStart).count();
                    if (taskUs > 5000) {
                        LOGI("X11Perf: task took %lld us", (long long)taskUs);
                    }
                    taskCount++;
                    lock.lock();
                }
            }

            // Call idle callback directly on this thread - all X11 operations must
            // happen on the pluginUI thread to prevent xcb_xlib_threads_sequence_lost
            {
                std::function<void()> callback;
                {
                    std::lock_guard<std::mutex> lock(idleCallbackMutex);
                    callback = idleCallback;
                }
                if (callback) {
                    auto idleStart = std::chrono::steady_clock::now();
                    callback();
                    auto idleUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - idleStart).count();
                    if (idleUs > 1000 || loopCount % 60 == 0) {
                        LOGI("X11Perf: idle took %lld us (loop #%d, tasks=%d)", (long long)idleUs, loopCount, taskCount);
                    }
                } else {
                    if (loopCount % 60 == 0) {
                        LOGI("X11Perf: no idle callback (loop #%d)", loopCount);
                    }
                }
            }

            auto loopUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - loopStart).count();
            if (loopUs > 20000 || loopCount % 120 == 0) {
                LOGI("X11Perf: loop #%d total %lld us (tasks=%d)", loopCount, (long long)loopUs, taskCount);
            }

            // Wait up to ~16ms for new tasks (or idle again)
            {
                std::unique_lock<std::mutex> lock(pluginUITaskMutex);
                pluginUITaskCv.wait_for(lock, std::chrono::milliseconds(16),
                    [this]() { return !pluginUITasks.empty() || !pluginUIRunning; });
            }
        }

        // Drain remaining tasks so postTaskAndWait callers are not blocked forever.
        // This handles the case where detachSurface() sets pluginUIRunning=false
        // while a task is queued but not yet processed.
        {
            std::unique_lock<std::mutex> lock(pluginUITaskMutex);
            while (!pluginUITasks.empty()) {
                auto task = std::move(pluginUITasks.front());
                pluginUITasks.pop();
                lock.unlock();
                task();
                lock.lock();
            }
        }

        LOGI("X11Debug: pluginUI thread exiting display=%d tid=%ld", displayNumber_, getTid());
    }
};

// Definition for the static thread_local member declared inside Impl.
// Each connection thread has its own clientFd. Threads outside connection
// handling (UI thread, render thread, etc.) see -1 — which is intentional;
// they don't read/write client sockets directly.
thread_local int X11NativeDisplay::Impl::clientFd = -1;
thread_local uint16_t X11NativeDisplay::Impl::lastReplySeq_ = 0;

static std::mutex g_displayMutex;
static std::unordered_map<int, std::unique_ptr<X11NativeDisplay>> g_displays;

X11NativeDisplay::X11NativeDisplay(int displayNumber)
    : displayNumber_(displayNumber) {
    impl_ = std::make_unique<Impl>();
    impl_->displayNumber_ = displayNumber;
    impl_->uiScale = 1.0f;
    LOGI("X11NativeDisplay: created display=%d with uiScale=%.2f", displayNumber, impl_->uiScale);
}


X11NativeDisplay::~X11NativeDisplay() {
    /* CRITICAL: signal teardown FIRST so the accept thread closes serverFd
     * and unblocks any per-client recv loops. Without this, the serverFd
     * (bound to port 6001/6002/…) leaks across plugin re-adds and the
     * NEXT wine subprocess's XOpenDisplay lands in the orphan's accept
     * backlog forever — the symptom the user reported as "editor doesn't
     * render after a TONEX/AmpliTube failed launch". signalDetach() drops
     * `running` and shuts down serverFd; serverLoop sees accept() return
     * < 0, breaks out of its loop, and closes serverFd at line ~4413. */
    signalDetach();
    /* Wait briefly for the server thread to wind down so port 6001 is
     * released before impl_ gets destroyed. Bounded so a stuck thread
     * can't block us forever — if it hasn't drained in 2s, we proceed
     * to the leak path below. */
    for (int i = 0; i < 200 && impl_->listening_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    detachSurface();
    /* If detach was deferred (plugin creation in progress), we must not destroy the Impl
     * yet because the render thread may still be using bufferMutex. Leak the Impl to
     * prevent use-after-free of the mutex. This is a memory leak but prevents the crash.
     * The display will be cleaned up later via nativeDetachAndDestroyX11DisplayIfExists. */
    if (impl_->detachDeferred.load()) {
        LOGI("X11Debug: ~X11NativeDisplay display=%d LEAKING Impl (detach was deferred)", displayNumber_);
        (void)impl_.release();  // Intentionally leak to prevent mutex use-after-free
    }
}

/* Start the X11 protocol server WITHOUT touching EGL / a Surface. After
 * this returns, wine clients can connect, send drawing requests, and our
 * framebuffer accumulates pixels — the on-screen render is deferred until
 * attachSurface arrives later. This split lets the UI mount its
 * SurfaceView only AFTER the plugin reports its real editor size, so the
 * SurfaceView aspect can be chosen correctly from the start (changing it
 * later crashes the Adreno EGL driver). */
int X11NativeDisplay::getActualPort() const {
    return impl_->actualPort_;
}

bool X11NativeDisplay::startServer(int placeholderW, int placeholderH) {
    if (impl_->running) {
        LOGI("X11 display %d: startServer skipped (already running)", displayNumber_);
        return true;
    }
    /* DEBUG: load the focus-emulation A/B mask once at startup (so variants that
     * affect load-time focus apply when the flag is written before launch). It
     * is also re-read on every touch-down for live toggling. */
    {
        uint32_t fm = impl_->readFocusMode();
        impl_->focusModeMask_.store(fm, std::memory_order_relaxed);
        if (fm) LOGI("X11 display %d focusMode=0x%x (from cache/x11_focus_mode.txt)", displayNumber_, fm);
    }
    /* Seed minimal EWMH window-manager state on the root BEFORE accepting a
     * client. Wine reads the root's _NET_SUPPORTED at X11 init
     * (net_supported_init) to decide is_net_supported(_NET_ACTIVE_WINDOW); if
     * TRUE it uses its EWMH activation path, otherwise a no-WM fallback that
     * fails to activate embedded CEF (Chromium) editor WebContents — so e.g.
     * BIAS FX 2's text fields never focus (no caret). Advertise _NET_ACTIVE_
     * WINDOW; the activation ClientMessages wine then sends are handled in the
     * SendEvent path. Atom IDs persist for the display's lifetime and match the
     * IDs wine gets from InternAtom (same store), so the property keys line up. */
    {
        uint32_t netSupported    = impl_->atoms_.intern("_NET_SUPPORTED", false);
        uint32_t netActiveWindow = impl_->atoms_.intern("_NET_ACTIVE_WINDOW", false);
        std::lock_guard<std::mutex> lk(impl_->propStoreMutex);
        auto& sup = impl_->propStore_[((uint64_t)kRootWindowId << 32) | netSupported];
        sup.type = 4 /*XA_ATOM*/; sup.format = 32;
        sup.data = { (uint8_t)(netActiveWindow & 0xff),  (uint8_t)((netActiveWindow >> 8) & 0xff),
                     (uint8_t)((netActiveWindow >> 16) & 0xff), (uint8_t)((netActiveWindow >> 24) & 0xff) };
        auto& act = impl_->propStore_[((uint64_t)kRootWindowId << 32) | netActiveWindow];
        act.type = 33 /*XA_WINDOW*/; act.format = 32;
        act.data = { 0, 0, 0, 0 };
        LOGI("X11 display %d EWMH seeded: _NET_SUPPORTED=%u _NET_ACTIVE_WINDOW=%u root=0x%x",
             displayNumber_, netSupported, netActiveWindow, kRootWindowId);
    }
    impl_->width  = placeholderW > 0 ? placeholderW : 1;
    impl_->height = placeholderH > 0 ? placeholderH : 1;
    impl_->framebuffer.assign((size_t)impl_->width * impl_->height, 0xFF302020);
    impl_->dirty = true;

    impl_->running = true;
    impl_->listening_ = false;
    impl_->pluginUIRunning = true;
    impl_->serverThread   = std::thread(&Impl::serverLoop,   impl_.get());
    impl_->pluginUIThread = std::thread(&Impl::pluginUILoop, impl_.get());

    for (int i = 0; i < 100 && !impl_->listening_; i++) usleep(10000);
    LOGI("X11 display %d server started (placeholder fb %dx%d, listening=%d)",
         displayNumber_, impl_->width, impl_->height, (int)impl_->listening_);
    return true;
}

bool X11NativeDisplay::attachSurface(JNIEnv* jniEnv, jobject jSurface, int width, int height) {
    if (!jniEnv || !jSurface) return false;

    /* Lazily start the protocol server if Android hasn't already. */
    if (!impl_->running) {
        startServer(width, height);
    }

    // Re-attach safe path. When the Surface is recreated (e.g. file picker
    // round-trip: MainActivity onPause → onResume causes Compose to dispose
    // and re-mount the SurfaceView), surfaceChanged fires a second time and
    // we land here while impl_->renderThread is still joinable from the
    // first attach. Move-assigning to a joinable std::thread calls
    // std::terminate (observed: "libc++abi: terminating" on main thread
    // immediately after the new render thread started). Drain the old
    // thread + EGL state first.
    if (impl_->renderThread.joinable()) {
        LOGI("X11Debug: attachSurface display=%d re-attach: draining prior render thread + EGL state", displayNumber_);
        impl_->renderThreadRunning = false;
        impl_->dirtyCv.notify_all();
        impl_->renderThread.join();
    }
    if (impl_->eglDisplay != EGL_NO_DISPLAY) {
        if (impl_->eglContext != EGL_NO_CONTEXT) {
            eglMakeCurrent(impl_->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(impl_->eglDisplay, impl_->eglContext);
            impl_->eglContext = EGL_NO_CONTEXT;
        }
        if (impl_->eglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(impl_->eglDisplay, impl_->eglSurface);
            impl_->eglSurface = EGL_NO_SURFACE;
        }
    }
    if (impl_->window) {
        ANativeWindow_release(impl_->window);
        impl_->window = nullptr;
    }

    ANativeWindow* win = ANativeWindow_fromSurface(jniEnv, jSurface);
    if (!win) {
        LOGE("ANativeWindow_fromSurface failed");
        return false;
    }

    impl_->width = width > 0 ? width : 1;
    impl_->height = height > 0 ? height : 1;
    ANativeWindow_setBuffersGeometry(win, impl_->width, impl_->height, 1 /* AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM */);

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        ANativeWindow_release(win);
        return false;
    }
    if (!eglInitialize(display, nullptr, nullptr)) {
        LOGE("eglInitialize failed");
        ANativeWindow_release(win);
        return false;
    }

    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLConfig config;
    EGLint numConfig;
    if (!eglChooseConfig(display, configAttribs, &config, 1, &numConfig) || numConfig == 0) {
        LOGE("eglChooseConfig failed");
        eglTerminate(display);
        ANativeWindow_release(win);
        return false;
    }

    EGLSurface eglSurf = eglCreateWindowSurface(display, config, win, nullptr);
    if (eglSurf == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed");
        eglTerminate(display);
        ANativeWindow_release(win);
        return false;
    }

    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttribs);
    if (ctx == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed");
        eglDestroySurface(display, eglSurf);
        eglTerminate(display);
        ANativeWindow_release(win);
        return false;
    }

    impl_->window = win;
    impl_->eglDisplay = display;
    impl_->eglSurface = eglSurf;
    impl_->eglContext = ctx;
    /* Don't reset the framebuffer here — it may already contain pixels
     * drawn by the wine client between startServer() and attachSurface(). */
    impl_->dirty = true;
    impl_->renderThreadRunning = true;
    impl_->renderThreadExited.store(false, std::memory_order_release);
    impl_->renderThread = std::thread(&Impl::renderLoop, impl_.get());

    LOGI("Display %d attached %dx%d", displayNumber_, impl_->width, impl_->height);
    return true;
}

bool X11NativeDisplay::signalDetach() {
    /* Graceful teardown strategy:
     * 1. Mark connection as "closing" instead of hard close
     * 2. Stop sending new events  
     * 3. Let client drain pending events
     * 4. Send synthetic DestroyNotify
     * 5. Close after timeout or client disconnect
     * 
     * Hard TCP/socket close while client is mid-request causes xcb_xlib_threads_sequence_lost.
     * This mirrors how real X servers behave.
     */
    
    // Check if plugin creation is in progress for this display - if so, defer closing the connection
    if (isCreatingPluginUIForDisplay(displayNumber_)) {
        LOGI("X11Debug: X11 signalDetach DEFERRED display=%d tid=%ld (plugin creation in progress)",
             displayNumber_, getTid());
        impl_->detachDeferred.store(true);
        return true;  // deferred
    }
    
    // Already closing gracefully? Just return
    if (impl_->closingGracefully.load()) {
        LOGI("X11Debug: X11 signalDetach already closing gracefully display=%d", displayNumber_);
        return false;
    }
    
    LOGI("X11Debug: X11 signalDetach ENTER display=%d tid=%ld clientFd=%d (initiating graceful teardown)",
         displayNumber_, getTid(), impl_->clientFd);

    // Mark as closing gracefully - server thread will handle the rest
    impl_->closingGracefully.store(true);
    impl_->closeStartTime = std::chrono::steady_clock::now();

    // Wake up the serverLoop's blocking accept() so serverThread.join()
    // can complete. The loop checks `running` after accept returns and
    // breaks out, then falls through to closing serverFd itself.
    impl_->running.store(false);
    if (impl_->serverFd >= 0) {
        ::shutdown(impl_->serverFd, SHUT_RDWR);
    }
    // Wake every per-client thread's blocking recv() by shutting down
    // its socket. Per-client threads are .detach()'ed inside serverLoop
    // and we can't join them — but they each pop themselves from
    // activeClientFds while holding activeClientsMutex right before exit.
    // If we let signalDetach return while those threads are still in
    // their request loops, the Impl gets destroyed and the cleanup lock
    // hits a destroyed mutex (FORTIFY abort observed 2026-05-18). The
    // wait loop below blocks the destructor until activeClientFds drains.
    {
        std::lock_guard<std::mutex> lk(impl_->activeClientsMutex);
        for (int fd : impl_->activeClientFds) {
            if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
        }
    }
    // Wake the renderLoop so renderThread.join() doesn't block on its
    // dirtyCv wait. Setting the flag false is what breaks the outer while.
    impl_->renderThreadRunning.store(false);
    impl_->dirtyCv.notify_all();

    // Wait up to ~1s for per-client threads to remove themselves. They
    // do `activeClientFds.erase(...)` while holding activeClientsMutex
    // immediately before exiting, so once the vector is empty all
    // detached threads have moved past any access to Impl members.
    for (int i = 0; i < 100; ++i) {
        {
            std::lock_guard<std::mutex> lk(impl_->activeClientsMutex);
            if (impl_->activeClientFds.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return false;  // not deferred, executed
}

void X11NativeDisplay::stopRenderThreadOnly() {
    LOGI("X11Debug: stopRenderThreadOnly display=%d tid=%ld (stopping render thread and waiting for exit)", displayNumber_, getTid());

    // Signal render thread to stop using the separate renderThreadRunning flag
    // This keeps the X11 server thread running (using 'running' flag)
    impl_->renderThreadRunning = false;
    impl_->dirtyCv.notify_all();  // Wake up render thread if waiting on dirty condition

    // Wait for render thread to actually exit with timeout
    // This prevents the "pthread_mutex_lock on destroyed mutex" crash by ensuring
    // no EGL operations are in progress when the surface is destroyed
    if (impl_->renderThread.joinable()) {
        LOGI("X11Debug: stopRenderThreadOnly display=%d waiting for render thread to exit...", displayNumber_);
        std::unique_lock<std::mutex> lock(impl_->renderExitMutex);
        bool exited = impl_->renderExitCv.wait_for(lock, std::chrono::milliseconds(500), [this]() {
            return impl_->renderThreadExited.load(std::memory_order_acquire);
        });
        if (exited) {
            LOGI("X11Debug: stopRenderThreadOnly display=%d render thread exited cleanly", displayNumber_);
        } else {
            LOGW("X11Debug: stopRenderThreadOnly display=%d timeout waiting for render thread exit", displayNumber_);
        }
    }
}

void X11NativeDisplay::startRenderThread() {
    // Serialize to prevent concurrent starts (e.g. resumeX11Display + requestFrame).
    std::lock_guard<std::mutex> guard(impl_->renderExitMutex);
    LOGI("X11Debug: startRenderThread display=%d tid=%ld (restarting render thread)", displayNumber_, getTid());

    // Check if render thread is already running
    if (impl_->renderThread.joinable() && !impl_->renderThreadExited.load(std::memory_order_acquire)) {
        LOGI("X11Debug: startRenderThread display=%d render thread already running", displayNumber_);
        // Just make sure running flags are set and request a frame
        impl_->renderThreadRunning = true;
        impl_->dirty = true;
        impl_->dirtyCv.notify_all();
        return;
    }

    // If thread has exited, we need to join it first before creating a new one
    if (impl_->renderThread.joinable() && impl_->renderThreadExited.load(std::memory_order_acquire)) {
        LOGI("X11Debug: startRenderThread display=%d joining exited render thread", displayNumber_);
        impl_->renderThread.join();
    }

    // Reset the exit flag and start a new render thread
    impl_->renderThreadExited.store(false, std::memory_order_release);
    impl_->renderThreadRunning = true;
    impl_->dirty = true;

    LOGI("X11Debug: startRenderThread display=%d starting new render thread, eglSurface=%p",
         displayNumber_, (void*)impl_->eglSurface);
    impl_->renderThread = std::thread(&Impl::renderLoop, impl_.get());

    LOGI("X11Debug: startRenderThread display=%d render thread restarted", displayNumber_);

    // Send Expose events to all known windows to force a full redraw.
    // Without this, after hide/resume the plugin only redraws areas that receive
    // input events (e.g. touched knobs) but the rest of the UI stays blank.
    impl_->sendExposeToAllWindows();
}

void X11NativeDisplay::detachSurface() {
    LOGI("X11Debug: X11 detachSurface ENTER display=%d tid=%ld",
         displayNumber_, getTid());
    /* Idempotent: if already detached (e.g. from destructor after explicit detach), skip */
    if (impl_->clientFd < 0 && !impl_->serverThread.joinable()) {
        LOGI("X11Close: X11 detachSurface display=%d already detached, skip", displayNumber_);
        return;
    }
    bool deferred = signalDetach();
    if (deferred) {
        LOGI("X11Debug: X11 detachSurface display=%d DEFERRED (plugin creation in progress), returning early", displayNumber_);
        return;
    }
    /* Join threads AFTER signalDetach has set running=false and closed fds.
     * This ensures the render thread has completely exited before we return,
     * so we don't race with EGL teardown in the render thread. */
    // Stop pluginUI thread first (it may hold references to plugin UI)
    impl_->pluginUIRunning = false;
    impl_->pluginUITaskCv.notify_all();
    if (impl_->pluginUIThread.joinable()) {
        LOGI("X11Debug: detachSurface display=%d joining pluginUIThread...", displayNumber_);
        impl_->pluginUIThread.join();
        LOGI("X11Debug: detachSurface display=%d pluginUIThread joined", displayNumber_);
    }
    if (impl_->serverThread.joinable()) {
        LOGI("X11Debug: detachSurface display=%d joining serverThread...", displayNumber_);
        impl_->serverThread.join();
        LOGI("X11Debug: detachSurface display=%d serverThread joined", displayNumber_);
    }
    if (impl_->renderThread.joinable()) {
        LOGI("X11Debug: detachSurface display=%d joining renderThread...", displayNumber_);
        impl_->renderThread.join();
        LOGI("X11Debug: detachSurface display=%d renderThread joined", displayNumber_);
    }
    if (impl_->window) {
        ANativeWindow_release(impl_->window);
        impl_->window = nullptr;
    }
    impl_->framebuffer.clear();
    /* Close the file descriptors AFTER threads have joined/exited.
     * This avoids the fdsan "double close" issue when signalDetach runs
     * on a different thread than the server thread. */
    if (impl_->clientFd >= 0) {
        close(impl_->clientFd);
        impl_->clientFd = -1;
    }
    if (impl_->serverFd >= 0) {
        close(impl_->serverFd);
        impl_->serverFd = -1;
    }
}

void X11NativeDisplay::setSurfaceSize(int width, int height) {
    if (width > 0 && height > 0) {
        std::lock_guard<std::mutex> lock(impl_->bufferMutex);
        impl_->width = width;
        impl_->height = height;
        // Only initialize framebuffer if no plugin content exists yet.
        // When plugin is already rendering, keep the existing framebuffer —
        // GL handles scaling to the new surface size via viewport.
        if (impl_->framebuffer.empty()) {
            int fw = impl_->pluginWidth > 0 ? impl_->pluginWidth : width;
            int fh = impl_->pluginHeight > 0 ? impl_->pluginHeight : height;
            uint32_t bgX11 = 0xFF302020;
            impl_->framebuffer.assign((size_t)fw * fh, bgX11);
        }
        impl_->dirty = true;
        impl_->dirtyCv.notify_one();  // Wake render thread to re-render at new size
        // Don't touch ANativeWindow_setBuffersGeometry here. Android's
        // SurfaceFlinger has already resized the buffer queue by the time
        // surfaceChanged fires; calling it again while the render thread is
        // mid-eglSwapBuffers crashes the Adreno GLES driver with SIGSEGV.
        // GL viewport in renderLoop already accommodates the new dimensions.
    }
}

void X11NativeDisplay::injectTouch(int action, int x, int y) {
    static int touchLogCount = 0;
    const int n = ++touchLogCount;
    const char* actionName = (action == 0) ? "DOWN"
                           : (action == 1) ? "UP"
                           : (action == 3) ? "RIGHTTAP"
                           : "MOVE";
    // Map from Android surface coordinates to the stacked framebuffer's
    // coordinate space. Framebuffer is pluginWidth × (pluginHeight * N)
    // where N = pluginSlotWindows.size(). Render is top-aligned (matches
    // X11NativeDisplay::renderLoop which sets y0=0), so the inverse map
    // here also uses y0=0.
    if (impl_->pluginWidth > 0 && impl_->width > 0 && impl_->height > 0) {
        if (n <= 3) {
            LOGI("DBG injectTouch[%d]: impl_->width=%d height=%d pluginW=%d pluginH=%d fbSizeFrozen=%d cropW=%d cropH=%d slotCount=%d",
                 displayNumber_, impl_->width, impl_->height,
                 impl_->pluginWidth, impl_->pluginHeight,
                 (int)impl_->fbSizeFrozen, impl_->cropW, impl_->cropH,
                 (int)impl_->pluginSlotWindows.size());
        }
        int slotCount = (int)impl_->pluginSlotWindows.size();
        int fbW = impl_->pluginWidth;
        int fbH = impl_->pluginHeight * (slotCount > 0 ? slotCount : 1);
        int cropOffX = 0, cropOffY = 0;
        /* Installer auto-crop mirrors renderLoop: surface→framebuffer must
         * use the SAME letterbox geometry as the renderer. When crop is
         * active, inverse-scale through (cropW, cropH) and add (cropX, cropY)
         * so the user's tap on the visible wizard lands at the correct
         * framebuffer coordinate. */
        if (impl_->fbSizeFrozen && impl_->cropW > 0 && impl_->cropH > 0) {
            cropOffX = impl_->cropX;
            cropOffY = impl_->cropY;
            fbW = impl_->cropW;
            fbH = impl_->cropH;
        }
        float scaleX = (float)impl_->width  / fbW;
        float scaleY = (float)impl_->height / fbH;
        float scale  = scaleX < scaleY ? scaleX : scaleY;
        int x0 = (impl_->width  - (int)(fbW * scale)) / 2;
        int y0 = 0;  // top-aligned render
        x = (int)((x - x0) / scale) + cropOffX;
        y = (int)((y - y0) / scale) + cropOffY;
        // y is in framebuffer coords. Resolve slot.
        if (slotCount > 0 && impl_->pluginHeight > 0) {
            int slot = y / impl_->pluginHeight;
            if (slot < 0) slot = 0;
            if (slot >= slotCount) slot = slotCount - 1;
            int yLocal = y - slot * impl_->pluginHeight;
            if (n <= 50 || n % 30 == 0) {
                LOGI("injectTouch slot=%d (of %d) yLocal=%d", slot, slotCount, yLocal);
            }
            y = yLocal;
            /* Stash the slot index for drainTouchQueue. The simplest
             * carrier is to bake it into a per-queue-entry field; but to
             * minimize churn now, we just remember the most-recent slot
             * via an atomic; drainTouchQueue picks it up. With one
             * physical touch source, races are negligible (events arrive
             * in order). */
            impl_->lastTouchSlot.store(slot, std::memory_order_release);
        }
    }
    if (n <= 50 || n % 30 == 0) {
        LOGI("injectTouch: display=%d action=%s (%d) plugin=(%d,%d) [call #%d]", displayNumber_, actionName, action, x, y, n);
    }
    /* injectTouch runs in the Android UI thread — impl_->clientFd reads
     * the UI thread's thread_local (always -1 since UI thread isn't a
     * connection thread). Gate on whether ANY connection has claimed
     * the editor window instead. The actual fd check happens in
     * drainTouchQueue, which runs in connection threads. */
    if (impl_->editorOwnerFd.load(std::memory_order_acquire) < 0) {
        /* Try to re-elect from the editor window's creator. JUCE-style
         * plugins create the window from the main process (which stays
         * alive) but the throwaway drawing process that flooded PutImage
         * may have disconnected, dropping the original claim. */
        uint32_t editorWid = 0;
        {
            std::lock_guard<std::mutex> lock(impl_->bufferMutex);
            if (!impl_->pluginSlotWindows.empty()) {
                editorWid = impl_->pluginSlotWindows[0];
            }
        }
        int candidateFd = -1;
        if (editorWid) {
            std::lock_guard<std::mutex> lock(impl_->windowCreatorMutex);
            auto it = impl_->windowCreator.find(editorWid);
            if (it != impl_->windowCreator.end()) candidateFd = it->second;
        }
        if (candidateFd >= 0) {
            std::lock_guard<std::mutex> lock(impl_->activeClientsMutex);
            bool stillAlive = std::find(impl_->activeClientFds.begin(),
                                        impl_->activeClientFds.end(),
                                        candidateFd) != impl_->activeClientFds.end();
            if (stillAlive) {
                impl_->editorOwnerFd.store(candidateFd, std::memory_order_release);
                LOGI("injectTouch: re-elected editor owner fd=%d (creator of wid=0x%x)",
                     candidateFd, editorWid);
            }
        }
        if (impl_->editorOwnerFd.load(std::memory_order_acquire) < 0) {
            if (n <= 10 || n % 50 == 0) LOGI("injectTouch: no editor owner yet, ignoring (display=%d)", displayNumber_);
            return;
        }
    }

    /* Queue the touch event for the server thread to send.
     * This prevents xcb_xlib_threads_sequence_lost: the X11 socket must only
     * be accessed from one thread (the server thread). Sending events directly
     * from the UI thread (via JNI) while the server thread is handling requests
     * causes the threading violation that crashes the app. */
    {
        std::lock_guard<std::mutex> lock(impl_->touchQueueMutex);
        if (action == 2 && !impl_->touchQueue.empty() && impl_->touchQueue.back().action == 2) {
            impl_->touchQueue.back().x = x;
            impl_->touchQueue.back().y = y;
        } else {
            impl_->touchQueue.push_back({action, x, y});
        }
    }
    if (n <= 80 || n % 50 == 0) {
        LOGI("injectTouch: queued action=%s (%d) at (%d,%d) for server thread", actionName, action, x, y);
    }
}

void X11NativeDisplay::injectKey(int action, int keycode, int state) {
    if (impl_->editorOwnerFd.load(std::memory_order_acquire) < 0) {
        static int n = 0;
        if (++n <= 5) LOGI("injectKey: no editor owner yet, ignoring (display=%d)", displayNumber_);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->keyQueueMutex);
        impl_->keyQueue.push_back({action, (uint8_t)keycode, (uint16_t)state});
    }
    static int n = 0;
    if (++n <= 80 || n % 50 == 0) {
        LOGI("injectKey: display=%d action=%s keycode=%d state=0x%x [#%d]",
             displayNumber_, action == 0 ? "DOWN" : "UP", keycode, state, n);
    }
}

bool X11NativeDisplay::isWidgetAtPoint(int surfaceX, int surfaceY) {
    // Map from Android surface coordinates to plugin coordinate space (same as injectTouch)
    int x = surfaceX, y = surfaceY;
    if (impl_->pluginWidth > 0 && impl_->width > 0 && impl_->height > 0) {
        float scaleX = (float)impl_->width / impl_->pluginWidth;
        float scaleY = (float)impl_->height / impl_->pluginHeight;
        float scale = scaleX < scaleY ? scaleX : scaleY;
        int x0 = (impl_->width  - (int)(impl_->pluginWidth  * scale)) / 2;
        int y0 = (impl_->height - (int)(impl_->pluginHeight * scale)) / 2;
        x = (int)((surfaceX - x0) / scale);
        y = (int)((surfaceY - y0) / scale);
    }
    // Hit-test with mutex (called from UI thread, maps modified by server thread)
    auto hit = impl_->hitTestChildWindow(x, y, /*lockMap=*/true);
    uint32_t topWin;
    size_t numChildren;
    {
        std::lock_guard<std::mutex> mapLock(impl_->windowMapMutex);
        topWin = impl_->childWindows.empty() ? kRootWindowId : impl_->childWindows[0];
        numChildren = impl_->childWindows.size();
    }
    // Single-window plugins (e.g. GxPlugins) render their entire UI into one window
    // with no child sub-windows. Any touch on the plugin window is interactive.
    // Multi-window plugins (e.g. xputty/Guitarix trunk) create sub-windows per widget;
    // only touches on sub-windows (not the top-level background) are interactive.
    bool isWidget;
    if (numChildren <= 1) {
        // Single-window plugin: entire surface is the UI
        isWidget = (hit.wid != kRootWindowId);
    } else {
        // Multi-window plugin: only sub-windows are widgets
        isWidget = (hit.wid != kRootWindowId && hit.wid != topWin);
    }
    return isWidget;
}

void X11NativeDisplay::requestFrame() {
    if (impl_->window != nullptr) {
        // Auto-restart render thread if it has exited (e.g., eglSwapBuffers failure
        // during screen off, or surface invalidation).  startRenderThread() handles
        // its own locking and is safe to call concurrently.
        if (impl_->renderThreadExited.load(std::memory_order_acquire)) {
            LOGI("X11Debug: requestFrame display=%d render thread dead, restarting", displayNumber_);
            startRenderThread();
            return;  // startRenderThread sets dirty=true
        }
        impl_->dirty = true;
        impl_->dirtyCv.notify_one();
    }
}

void X11NativeDisplay::setIdleCallback(IdleCallback callback) {
    std::lock_guard<std::mutex> lock(impl_->idleCallbackMutex);
    impl_->idleCallback = std::move(callback);
}

void X11NativeDisplay::postTask(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(impl_->pluginUITaskMutex);
        impl_->pluginUITasks.push(std::move(task));
    }
    impl_->pluginUITaskCv.notify_one();
}

void X11NativeDisplay::postTaskAndWait(std::function<void()> task) {
    std::promise<void> done;
    std::future<void> fut = done.get_future();
    {
        std::lock_guard<std::mutex> lock(impl_->pluginUITaskMutex);
        impl_->pluginUITasks.push([&task, &done]() {
            task();
            done.set_value();
        });
    }
    impl_->pluginUITaskCv.notify_one();
    fut.get();
}

bool X11NativeDisplay::isAttached() const {
    return impl_->window != nullptr;
}

bool X11NativeDisplay::isUnixSocket() const {
    return impl_->useUnixSocket_;
}

bool X11NativeDisplay::getPluginSize(int& w, int& h) {
    std::lock_guard<std::mutex> lock(impl_->bufferMutex);
    if (impl_->pluginWidth > 0 && impl_->pluginHeight > 0) {
        w = impl_->pluginWidth;
        h = impl_->pluginHeight;
        return true;
    }
    return false;
}

void X11NativeDisplay::getRenderedExtent(int& w, int& h) {
    std::lock_guard<std::mutex> lock(impl_->bufferMutex);
    w = impl_->renderedMaxX_;
    h = impl_->renderedMaxY_;
}

/* Read-only snapshot of the rendered framebuffer for the desktop harness.
 * The framebuffer width is always pluginWidth (or the placeholder width
 * before a plugin is known); the height is the remainder of the vector so
 * the slot-stacked case (height = pluginHeight * slotCount) reports the full
 * stack and out.size() always equals w*h regardless of the resize path that
 * last touched the vector. */
bool X11NativeDisplay::snapshotFramebuffer(std::vector<uint32_t>& out, int& w, int& h) {
    std::lock_guard<std::mutex> lock(impl_->bufferMutex);
    if (impl_->framebuffer.empty()) {
        w = 0;
        h = 0;
        return false;
    }
    int fw = impl_->pluginWidth > 0 ? impl_->pluginWidth : impl_->width;
    if (fw <= 0) return false;
    int fh = (int)(impl_->framebuffer.size() / (size_t)fw);
    if (fh <= 0) return false;
    out.assign(impl_->framebuffer.begin(),
               impl_->framebuffer.begin() + (size_t)fw * fh);
    w = fw;
    h = fh;
    return true;
}

/* Forcibly set the plugin's native dimensions. Used when Android already
 * knows the editor size (from vst_host) and Wine's CreateWindow trail
 * didn't hit our auto-detect heuristic (JUCE creates the editor as a
 * child of a tiny placeholder, so the parent==root check never fires).
 * Resizes the framebuffer to match so the renderLoop's letterbox math
 * lands on the real aspect. */
void X11NativeDisplay::setFramebufferFrozen(bool frozen) {
    std::lock_guard<std::mutex> lock(impl_->bufferMutex);
    impl_->fbSizeFrozen = frozen;
    // Always reset crop state on freeze transitions. We saw stale crop
    // (cropW=1328 cropH=996 from a prior session) leaking into a fresh
    // installer's Impl, blocking detection because newArea < curArea on
    // every wizard ConfigureWindow. Explicit reset eliminates the
    // mystery, regardless of how the stale state got there.
    impl_->cropX = 0;
    impl_->cropY = 0;
    impl_->cropW = 0;
    impl_->cropH = 0;
    impl_->cropWid = 0;
    LOGI("X11 display %d: fbSizeFrozen = %d (crop reset)", displayNumber_, frozen ? 1 : 0);
}

void X11NativeDisplay::setPluginSize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    std::lock_guard<std::mutex> lock(impl_->bufferMutex);
    if (impl_->pluginWidth == w && impl_->pluginHeight == h) {
        return;
    }
    // Resize the framebuffer to (w × h) PRESERVING existing pixels at
    // overlap. Plugin's initial chrome paint may have landed in the
    // placeholder fb (800x600 at startServer time); a naive assign-to-
    // brown wipes that paint and the chrome shows brown until something
    // repaints it — but many plugins paint chrome once on init and
    // never again, so the brown holes are permanent. Copy the rows that
    // fit, leave the rest brown.
    //
    // Slots intentionally NOT cleared: see commit log for the race that
    // happens when the editor's CreateWindow lands AFTER setPluginSize
    // and parent-of-existing-slot lookup fails.
    const int oldW = impl_->pluginWidth > 0 ? impl_->pluginWidth : impl_->width;
    const int oldH = impl_->pluginHeight > 0 ? impl_->pluginHeight : impl_->height;
    constexpr uint32_t bgX11 = 0xFF302020;
    std::vector<uint32_t> newFb((size_t)w * h, bgX11);
    const int copyW = std::min(oldW, w);
    const int copyH = std::min(oldH, h);
    if (oldW > 0 && oldH > 0 && copyW > 0 && copyH > 0 &&
        impl_->framebuffer.size() == (size_t)oldW * oldH) {
        for (int yy = 0; yy < copyH; ++yy) {
            std::memcpy(&newFb[(size_t)yy * w],
                        &impl_->framebuffer[(size_t)yy * oldW],
                        (size_t)copyW * sizeof(uint32_t));
        }
    }
    impl_->framebuffer = std::move(newFb);
    impl_->pluginWidth = w;
    impl_->pluginHeight = h;
    impl_->dirty = true;
    impl_->dirtyCv.notify_one();
    LOGI("X11 display %d: plugin size set to %dx%d (slots kept: %zu, oldFb=%dx%d)",
         displayNumber_, w, h, impl_->pluginSlotWindows.size(), oldW, oldH);
}

void X11NativeDisplay::setUIScale(float scale) {
    impl_->uiScale = scale;
    LOGI("X11NativeDisplay: setUIScale(%.2f) for display=%d", scale, displayNumber_);
}

float X11NativeDisplay::getUIScale() const {
    return impl_->uiScale;
}

X11NativeDisplay* getOrCreateX11Display(int displayNumber) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    if (it != g_displays.end())
        return it->second.get();
    auto d = std::make_unique<X11NativeDisplay>(displayNumber);
    X11NativeDisplay* p = d.get();
    g_displays[displayNumber] = std::move(d);
    return p;
}

X11NativeDisplay* getX11Display(int displayNumber) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    return (it != g_displays.end()) ? it->second.get() : nullptr;
}

void destroyX11Display(int displayNumber) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    g_displays.erase(displayNumber);
}

int withDisplayGetActualPort(int displayNumber) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    return (it != g_displays.end()) ? it->second->getActualPort() : -1;
}

void withDisplayRequestFrame(int displayNumber) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    if (it != g_displays.end())
        it->second->requestFrame();
}

void withDisplayInjectTouch(int displayNumber, int action, int x, int y) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    if (it != g_displays.end())
        it->second->injectTouch(action, x, y);
}

void withDisplayInjectKey(int displayNumber, int action, int keycode, int state) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    if (it != g_displays.end())
        it->second->injectKey(action, keycode, state);
}

void withDisplaySetSurfaceSize(int displayNumber, int width, int height) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    if (it != g_displays.end())
        it->second->setSurfaceSize(width, height);
}

void withDisplaySetPluginSize(int displayNumber, int w, int h) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    if (it != g_displays.end())
        it->second->setPluginSize(w, h);
}

void withDisplaySetFramebufferFrozen(int displayNumber, bool frozen) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    if (it != g_displays.end()) {
        it->second->setFramebufferFrozen(frozen);
    }
}

void withDisplayStartServer(int displayNumber, int placeholderW, int placeholderH) {
    X11NativeDisplay* disp = getOrCreateX11Display(displayNumber);
    if (disp) disp->startServer(placeholderW, placeholderH);
}

bool withDisplayGetPluginSize(int displayNumber, int& w, int& h) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    if (it != g_displays.end())
        return it->second->getPluginSize(w, h);
    return false;
}

bool withDisplayGetRenderedExtent(int displayNumber, int& w, int& h) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    if (it != g_displays.end()) {
        it->second->getRenderedExtent(w, h);
        return true;
    }
    w = 0; h = 0;
    return false;
}

bool withDisplaySnapshotFramebuffer(int displayNumber, std::vector<uint32_t>& out,
                                    int& w, int& h) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    if (it != g_displays.end())
        return it->second->snapshotFramebuffer(out, w, h);
    w = 0; h = 0;
    return false;
}

float withDisplayGetUIScale(int displayNumber) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    if (it != g_displays.end())
        return it->second->getUIScale();
    return 1.0f;
}

bool withDisplayIsWidgetAtPoint(int displayNumber, int x, int y) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    if (it != g_displays.end())
        return it->second->isWidgetAtPoint(x, y);
    return false;
}

void withDisplayPostTask(int displayNumber, std::function<void()> task) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    if (it != g_displays.end())
        it->second->postTask(std::move(task));
}

void withDisplaySetIdleCallback(int displayNumber, X11NativeDisplay::IdleCallback cb) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    if (it != g_displays.end())
        it->second->setIdleCallback(std::move(cb));
}

bool withDisplayPostTaskAndWait(int displayNumber, std::function<void()> task) {
    X11NativeDisplay* disp = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_displayMutex);
        auto it = g_displays.find(displayNumber);
        if (it == g_displays.end())
            return false;
        disp = it->second.get();
    }
    disp->postTaskAndWait(std::move(task));
    return true;
}

} // namespace guitarrackcraft
