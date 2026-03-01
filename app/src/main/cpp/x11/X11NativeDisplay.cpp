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

#define LOG_TAG "X11NativeDisplay"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

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
    int clientFd = -1;
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
    // Render thread exit synchronization
    std::atomic<bool> renderThreadExited{false};
    std::mutex renderExitMutex;
    std::condition_variable renderExitCv;
    GLuint program = 0;
    GLuint texUniform = 0;
    GLuint fbTex = 0;    // persistent framebuffer texture (avoid per-frame alloc)
    GLuint fbVbo = 0;    // persistent vertex buffer
    bool msbFirst_ = true;  // X11 byte order: true = MSB first (0x42), false = LSB first (0x6c)
    std::atomic<bool> listening_{false};
    std::atomic<uint16_t> lastSeq_{0};  // last request sequence number, for event injection
    std::atomic<uint16_t> lastReplySeq_{0};  // sequence of last REPLY sent (not void requests)
    std::mutex writeMutex;  // protects writes to clientFd (replies + injected events)
    std::vector<uint32_t> childWindows;  // window IDs created by client (for event delivery)
    std::atomic<int> lastPointerX{0};     // last injected pointer X position (for QueryPointer)
    std::atomic<int> lastPointerY{0};     // last injected pointer Y position (for QueryPointer)
    std::atomic<bool> pointerButton1Down{false};  // button 1 state (for QueryPointer)
    std::unordered_map<uint32_t, uint32_t> windowEventMasks;  // window ID -> event mask (for pointer event filtering)

    /* Atom table: InternAtom maps name -> unique ID; GetAtomName maps ID -> name. */
    std::unordered_map<std::string, uint32_t> atomNameToId;
    std::unordered_map<uint32_t, std::string> atomIdToName;
    uint32_t nextAtomId = 1;  // atoms start at 1 (0 = None)

    /* Queued touch events: injected from UI thread, drained from server thread between requests.
     * This prevents xcb_xlib_threads_sequence_lost: events must not arrive on the socket while
     * the client-side libX11/XCB is in the middle of a request/reply exchange. */
    struct QueuedTouch {
        int action; // 0=down, 1=up, 2=move
        int x, y;
    };
    std::mutex touchQueueMutex;
    std::vector<QueuedTouch> touchQueue;
    std::mutex windowMapMutex;  // protects childWindows, windowPositions, windowSizes (read from UI thread via isWidgetAtPoint)
    std::unordered_map<uint32_t, std::pair<int, int>> windowSizes;  // window ID -> (width, height) for GetGeometry
    struct WindowPos { int x = 0, y = 0; uint32_t parent = 0; };
    std::unordered_map<uint32_t, WindowPos> windowPositions;  // window ID -> position relative to parent
    std::unordered_set<uint32_t> unmappedWindows;  // windows hidden via UnmapWindow
    int originalChildW = 0;  // original child window size from CreateWindow (before any scaling)
    int originalChildH = 0;
    int pluginWidth = 0;   // plugin's current window width (may be scaled)
    int pluginHeight = 0;  // plugin's current window height
    float uiScale = 1.0f;  // UI scale factor for plugin rendering (< 1.0 = smaller = faster)

    // Pixmap tracking: maps pixmap/window ID -> pixel buffer (BGRA, 4 bytes/pixel)
    struct PixmapData {
        int w = 0, h = 0;
        std::vector<uint32_t> pixels;  // same format as framebuffer (ARGB)
    };
    std::unordered_map<uint32_t, PixmapData> pixmaps;  // offscreen pixmaps

    // Compute absolute position of a window by walking parent chain
    // Returns offset relative to the top-level plugin window (childWindows[0])
    std::pair<int, int> getAbsolutePos(uint32_t wid) const {
        int ax = 0, ay = 0;
        uint32_t cur = wid;
        for (int depth = 0; depth < 32; depth++) {  // prevent infinite loop
            auto it = windowPositions.find(cur);
            if (it == windowPositions.end()) break;
            ax += it->second.x;
            ay += it->second.y;
            cur = it->second.parent;
            // Stop when we reach the top-level plugin window
            if (!childWindows.empty() && cur == childWindows[0]) break;
            if (cur == kRootWindowId || cur == 0) break;
        }
        return {ax, ay};
    }

    uint16_t read16(const uint8_t* p, int off) const {
        if (msbFirst_) return (uint16_t)((p[off] << 8) | p[off + 1]);
        return (uint16_t)(p[off] | (p[off + 1] << 8));
    }
    uint32_t read32(const uint8_t* p, int off) const {
        if (msbFirst_) return (uint32_t)((p[off]<<24)|(p[off+1]<<16)|(p[off+2]<<8)|p[off+3]);
        return (uint32_t)(p[off]|(p[off+1]<<8)|(p[off+2]<<16)|(p[off+3]<<24));
    }
    void write16(uint8_t* p, int off, uint16_t val) const {
        if (msbFirst_) { p[off] = (val >> 8) & 0xff; p[off+1] = val & 0xff; }
        else { p[off] = val & 0xff; p[off+1] = (val >> 8) & 0xff; }
    }
    void write32(uint8_t* p, int off, uint32_t val) const {
        if (msbFirst_) {
            p[off] = (val >> 24) & 0xff; p[off+1] = (val >> 16) & 0xff;
            p[off+2] = (val >> 8) & 0xff; p[off+3] = val & 0xff;
        } else {
            p[off] = val & 0xff; p[off+1] = (val >> 8) & 0xff;
            p[off+2] = (val >> 16) & 0xff; p[off+3] = (val >> 24) & 0xff;
        }
    }

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
            {
                std::lock_guard<std::mutex> lock(bufferMutex);
                fw = pluginWidth > 0 ? pluginWidth : width;
                fh = pluginHeight > 0 ? pluginHeight : height;
                if (width > 0 && height > 0 && !framebuffer.empty()) {
                    if (renderBuffer.size() != framebuffer.size()) {
                        renderBuffer.resize(framebuffer.size());
                    }
                    renderBuffer = framebuffer;  // Copy to staging buffer
                }
            }
            dirty = false;  // Clear after snapshot; new PutImage during render will re-set it

            // Render from staging buffer (no lock held - PutImage can update framebuffer concurrently)
            if (width > 0 && height > 0 && fw > 0 && fh > 0 && !renderBuffer.empty()) {
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

                // Render plugin texture in letterbox rect
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

    bool sendAllLocked(int fd, const void* data, size_t len) {
        std::lock_guard<std::mutex> lock(writeMutex);
        return sendAll(fd, data, len);
    }

    /** Send a reply and update lastReplySeq_ for correct event sequence tracking. */
    bool sendReply(const void* data, size_t len, uint16_t replySeq) {
        bool ok = sendAllLocked(clientFd, data, len);
        if (ok) {
            lastReplySeq_.store(replySeq, std::memory_order_relaxed);
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
    static bool recvAll(int fd, void* data, size_t len) {
        uint8_t* p = static_cast<uint8_t*>(data);
        while (len) {
            ssize_t n = recv(fd, p, len, 0);
            if (n > 0) {
                p += n;
                len -= n;
            } else if (n == 0) {
                return false;  // peer closed
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Non-blocking socket: wait for data with poll
                    struct pollfd pfd = { fd, POLLIN, 0 };
                    int ret = poll(&pfd, 1, 5000 /* 5s timeout */);
                    if (ret <= 0 || (pfd.revents & (POLLERR | POLLHUP))) return false;
                    continue;
                }
                return false;  // real error
            }
        }
        return true;
    }

    // X11 Connection Setup reply (success). Layout must match libX11/XCB parsing.
    // See X11 Protocol "Connection Setup" and libX11 OpenDis.c.
    void sendConnectionReply() {
        uint8_t body[120];  // 8 (header) + 112 (setup) = 120
        size_t off = 0;
#define APPEND(p, n) do { memcpy(body + off, (p), (n)); off += (n); } while(0)

        // 0) Reply header (8 bytes)
        body[off++] = kX11ConnectionAccepted;
        body[off++] = 0;
        write16(body + off, 0, kX11Major); off += 2;
        write16(body + off, 0, kX11Minor); off += 2;
        write16(body + off, 0, 28); off += 2;  // length in 4-byte units
        // 1) Fixed setup prefix (18 bytes)
        write32(body + off, 0, 0);          // release-number
        write32(body + off, 4, 0x00200000); // resource-id-base (disjoint from mask)
        write32(body + off, 8, 0x001FFFFF); // resource-id-mask (21 bits, disjoint from base)
        write32(body + off, 12, 256);       // motion-buffer-size
        write16(body + off, 16, 0);         // vendor-length
        off += 18;
        // 2) Match android-xserver order: max_request_length (2), num_screens (1), num_formats (1), image/bitmap/keycode (6), pad (4) = 14 → prefix total 32
        write16(body + off, 0, 32767); off += 2;   // max_request_length (0x7fff)
        body[off++] = 1;   // num roots (screens)
        body[off++] = 1;   // num formats
        body[off++] = msbFirst_ ? 1 : 0;  // image byte order: 0=LSBFirst, 1=MSBFirst
        body[off++] = msbFirst_ ? 1 : 0;  // bitmap bit order: 0=LeastSignificant, 1=MostSignificant
        body[off++] = 8; body[off++] = 8;  // bitmap scanline unit, pad
        body[off++] = 8; body[off++] = 255; // min/max keycode (min must be >= 8 per X11 spec)
        body[off++] = 0; body[off++] = 0; body[off++] = 0; body[off++] = 0;  // pad 4
        // 4) PixmapFormat (8)
        body[off++] = 24; body[off++] = 32;
        write16(body + off, 0, 32); off += 2;
        memset(body + off, 0, 4); off += 4;
        // 5) WindowRoot (40)
        write32(body + off, 0, kRootWindowId);
        write32(body + off, 4, kDefaultColormapId);
        write32(body + off, 8, kWhitePixel);
        write32(body + off, 12, kBlackPixel);
        write32(body + off, 16, 0);
        write16(body + off, 20, (uint16_t)width);
        write16(body + off, 22, (uint16_t)height);
        write16(body + off, 24, (uint16_t)(width * 254 / 100));
        write16(body + off, 26, (uint16_t)(height * 254 / 100));
        write16(body + off, 28, 0);
        write16(body + off, 30, 0);
        write32(body + off, 32, kDefaultVisualId);  // root_visual
        body[off + 36] = 0; body[off + 37] = 0; body[off + 38] = 24; body[off + 39] = 1;
        off += 40;
        // 6) Depth (8) + VisualType (24)
        body[off++] = 24; body[off++] = 0;
        write16(body + off, 0, 1); off += 2;
        memset(body + off, 0, 4); off += 4;
        write32(body + off, 0, kDefaultVisualId);  // visual ID (must be non-zero for Xlib lookups)
        body[off + 4] = 4; body[off + 5] = 32;
        write16(body + off, 6, 256);
        write32(body + off, 8, 0xff0000);
        write32(body + off, 12, 0x00ff00);
        write32(body + off, 16, 0x0000ff);
        memset(body + off + 20, 0, 4);
        off += 24;
        // 7) No extra pad — total must be exactly 120 (8 + 112) so libX11 usedbytes == setuplength
        if (off != 120) LOGE("X11 connection reply: wrong size %zu (expected 120)", off);
#undef APPEND
        LOGI("X11 connection reply: sending %zu bytes (header+setup)", off);
        logHex("X11 reply", body, off);

        if (!sendAllLocked(clientFd,body, off)) {
            LOGE("X11 connection reply: send failed");
            return;
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
        uint16_t evtSeq = lastReplySeq_.load(std::memory_order_relaxed);

        auto sendExpose = [&](uint32_t wid) {
            int w = width, h = height;
            auto it = windowSizes.find(wid);
            if (it != windowSizes.end()) {
                w = it->second.first;
                h = it->second.second;
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
    std::chrono::steady_clock::time_point lastExposeFlush = std::chrono::steady_clock::now();
    static constexpr double FLUSH_INTERVAL_SEC = kTouchFlushIntervalSec;

    // Send event to a specific child window with local coordinates
    void sendEventToChild(uint8_t type, int globalX, int globalY, int button, uint16_t seq, int stateOverride = -1) {
        HitResult hit;
        if (grabWindow != 0) {
            // During drag, keep sending to the grabbed window
            auto absPos = getAbsolutePos(grabWindow);
            hit = {grabWindow, globalX - absPos.first, globalY - absPos.second};
        } else {
            hit = hitTestChildWindow(globalX, globalY);
        }
        sendEvent(type, hit.wid, hit.localX, hit.localY, button, seq, stateOverride);
    }

    void drainTouchQueue() {
        std::vector<QueuedTouch> pending;
        {
            std::lock_guard<std::mutex> lock(touchQueueMutex);
            pending.swap(touchQueue);
        }

        uint16_t seq = lastReplySeq_.load(std::memory_order_relaxed);

        // Process button press/release immediately; buffer drag moves
        for (auto& t : pending) {
            lastPointerX.store(t.x, std::memory_order_relaxed);
            lastPointerY.store(t.y, std::memory_order_relaxed);
            if (t.action == 0) {
                // Flush any pending drag before ButtonPress
                if (hasPendingDrag) {
                    sendEventToChild(MotionNotify, pendingDragX, pendingDragY, 0, seq);
                    hasPendingDrag = false;
                }
                // Hit-test and grab the child window
                HitResult hit = hitTestChildWindow(t.x, t.y);
                grabWindow = hit.wid;
                // Send synthetic MotionNotify before ButtonPress so that
                // widgets like combobox popups set prelight_item from the
                // touch position (on desktop, mouse hover does this).
                // state=0 means no buttons are pressed, avoiding drag logic.
                sendEvent(MotionNotify, hit.wid, hit.localX, hit.localY, 0, seq, 0);
                pointerButton1Down.store(true, std::memory_order_relaxed);
                sendEvent(ButtonPress, hit.wid, hit.localX, hit.localY, 1, seq);
            } else if (t.action == 1) {
                // Flush any pending drag before ButtonRelease
                if (hasPendingDrag) {
                    sendEventToChild(MotionNotify, pendingDragX, pendingDragY, 0, seq);
                    hasPendingDrag = false;
                }
                pointerButton1Down.store(false, std::memory_order_relaxed);
                sendEventToChild(ButtonRelease, t.x, t.y, 1, seq);
                grabWindow = 0;  // Release grab
            } else {
                // Drag move: just buffer the latest position
                hasPendingDrag = true;
                pendingDragX = t.x;
                pendingDragY = t.y;
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
    }

    // Hit-test: find the deepest child window containing (x,y) in framebuffer coords.
    // Returns {windowId, localX, localY} where local coords are relative to the found window.
    struct HitResult { uint32_t wid; int localX; int localY; };
    HitResult hitTestChildWindow(int x, int y, bool lockMap = false) {
        std::unique_lock<std::mutex> mapLock(windowMapMutex, std::defer_lock);
        if (lockMap) mapLock.lock();
        // Default: top-level plugin window (or root)
        uint32_t topWin = childWindows.empty() ? kRootWindowId : childWindows[0];
        HitResult best = {topWin, x, y};

        // Walk child windows in reverse order (last created = topmost) to find deepest hit
        // Skip childWindows[0] which is the top-level plugin window
        for (int i = (int)childWindows.size() - 1; i >= 1; i--) {
            uint32_t wid = childWindows[i];
            if (unmappedWindows.count(wid)) continue;  // skip hidden windows
            auto posIt = windowPositions.find(wid);
            auto sizeIt = windowSizes.find(wid);
            if (posIt == windowPositions.end() || sizeIt == windowSizes.end()) continue;

            auto absPos = getAbsolutePos(wid);
            int wx = absPos.first, wy = absPos.second;
            int ww = sizeIt->second.first, wh = sizeIt->second.second;

            if (x >= wx && x < wx + ww && y >= wy && y < wy + wh) {
                best = {wid, x - wx, y - wy};
                break;  // Found deepest (topmost) hit
            }
        }
        return best;
    }

    uint32_t x11Timestamp() {
        using namespace std::chrono;
        return (uint32_t)duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
    }

    void sendEvent(uint8_t type, uint32_t windowId, int x, int y, int button, uint16_t lastSeq, int stateOverride = -1) {
        uint8_t buf[32];
        memset(buf, 0, 32);
        buf[0] = type;
        buf[1] = (type == ButtonPress || type == ButtonRelease)
                 ? (uint8_t)button : 0;        // detail (button# or 0)
        write16(buf, 2, lastSeq);               // sequence number
        write32(buf, 4, x11Timestamp());        // time
        write32(buf, 8, kRootWindowId);        // root window
        write32(buf, 12, windowId);             // event window
        write32(buf, 16, 0);                    // child window (None)
        write16(buf, 20, (uint16_t)(int16_t)x); // root-x
        write16(buf, 22, (uint16_t)(int16_t)y); // root-y
        write16(buf, 24, (uint16_t)(int16_t)x); // event-x
        write16(buf, 26, (uint16_t)(int16_t)y); // event-y
        uint16_t state = 0;
        if (stateOverride >= 0) {
            state = (uint16_t)stateOverride;
        } else {
            if (type == ButtonRelease && button == 1) state = (1 << 8); // Button1Mask
            if (type == MotionNotify) state = (1 << 8); // Button1Mask while dragging
        }
        write16(buf, 28, state);                // state
        buf[30] = 1;                            // same-screen = True
        sendAllLocked(clientFd,buf, 32);
    }

    void serverLoop() {
        /* Always use TCP loopback. Our custom-built libxcb does not support
         * abstract Unix sockets — it tries filesystem path /tmp/.X11-unix/XN
         * which doesn't exist on Android. TCP 127.0.0.1:(6000+N) works reliably. */
        bool useUnix = false;

        {
            int port = kX11BasePort + displayNumber_;
            serverFd = socket(AF_INET, SOCK_STREAM, 0);
            if (serverFd < 0) {
                LOGE("socket failed: %s", strerror(errno));
                return;
            }
            int one = 1;
            setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            struct sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<uint16_t>(port));
            inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            if (bind(serverFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
                LOGE("bind 127.0.0.1:%d failed: %s", port, strerror(errno));
                close(serverFd);
                serverFd = -1;
                return;
            }
            if (listen(serverFd, 1) < 0) {
                LOGE("listen failed");
                close(serverFd);
                serverFd = -1;
                return;
            }
            LOGI("X11 server listening on 127.0.0.1:%d (TCP fallback)", kX11BasePort + displayNumber_);
        }
        listening_ = true;
        useUnixSocket_ = useUnix;

        while (running) {
            clientFd = accept(serverFd, nullptr, nullptr);
            if (clientFd < 0 || !running) break;

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
            LOGI("X11 accept: client connected (%s)", useUnixSocket_ ? "Unix" : "TCP");
            {
                std::lock_guard<std::mutex> mapLock(windowMapMutex);
                childWindows.clear();
                windowSizes.clear();
                windowPositions.clear();
                unmappedWindows.clear();
            }
            pixmaps.clear();

            uint8_t req[12];
            if (!recvAll(clientFd, req, 12)) {
                LOGE("X11 accept: failed to recv 12-byte connection request");
                close(clientFd);
                clientFd = -1;
                continue;
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
                            write16(evt, 2, lastReplySeq_.load(std::memory_order_relaxed));
                            write32(evt, 4, wid);
                            write32(evt, 8, wid);
                            {
                                std::lock_guard<std::mutex> lock(writeMutex);
                                sendAll(clientFd, evt, 32);
                            }
                            seq++;
                            lastSeq_.store(seq, std::memory_order_relaxed);
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
                    continue;
                }

                seq++;
                lastSeq_.store(seq, std::memory_order_relaxed);

                thread_local int reqLogCount = 0;
                thread_local bool seenOpcode[256] = {};
                ++reqLogCount;
                if (!seenOpcode[opcode]) {
                    seenOpcode[opcode] = true;
                    LOGI("X11 opcode first seen: #%d opcode=%u %s length=%u seq=%u tid=%ld",
                         reqLogCount, (unsigned)opcode, x11OpcodeName(opcode), (unsigned)length, (unsigned)seq, getTid());
                } else if (reqLogCount <= 100) {
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
                            if (isWindow && !isTopLevel && unmappedWindows.count(drawable)) {
                                if (reqLogCount <= 100) LOGI("X11 PutImage SKIP unmapped wid=0x%x", drawable);
                                isWindow = false;  // suppress framebuffer write
                            }

                            /* For child windows, offset PutImage coords by window's absolute position */
                            if (isWindow && !isTopLevel) {
                                auto absPos = getAbsolutePos(drawable);
                                x += absPos.first;
                                y += absPos.second;
                            }

                            /* Collect mapped child window rects to clip parent drawing.
                             * On a real X11 server, child windows float above parents. On our
                             * single-framebuffer server, we simulate this by skipping parent
                             * pixels that fall within mapped child window bounds. */
                            struct ClipRect { int x1, y1, x2, y2; };
                            static thread_local std::vector<ClipRect> childClip;
                            childClip.clear();
                            if (isWindow) {
                                std::lock_guard<std::mutex> mapLock(windowMapMutex);
                                for (auto& [cwid, cpos] : windowPositions) {
                                    if (cpos.parent == drawable && !unmappedWindows.count(cwid)) {
                                        auto csizeIt = windowSizes.find(cwid);
                                        if (csizeIt != windowSizes.end()) {
                                            auto cabs = getAbsolutePos(cwid);
                                            childClip.push_back({
                                                cabs.first, cabs.second,
                                                cabs.first + csizeIt->second.first,
                                                cabs.second + csizeIt->second.second
                                            });
                                        }
                                    }
                                }
                            }

                            uint32_t* dstBuf = nullptr;
                            int dW = 0, dH = 0;
                            int fbw = pluginWidth > 0 ? pluginWidth : width;
                            int fbh = pluginHeight > 0 ? pluginHeight : height;
                            if (isWindow && framebuffer.size() == (size_t)fbw * fbh) {
                                dstBuf = framebuffer.data(); dW = fbw; dH = fbh;
                            } else {
                                auto it = pixmaps.find(drawable);
                                if (it != pixmaps.end()) {
                                    dstBuf = it->second.pixels.data();
                                    dW = it->second.w; dH = it->second.h;
                                }
                            }

                            if (dstBuf) {
                                /* Fast path: region fully inside destination, LSB-first, complete pixel data */
                                bool fullyCovered = (x >= 0 && y >= 0 && x + w <= dW && y + h <= dH
                                    && pixelDataLen >= (size_t)w * h * 4);
                                if (fullyCovered && !msbFirst_ && childClip.empty()) {
                                    const uint32_t* src32 = reinterpret_cast<const uint32_t*>(pixels.data());
                                    // Framebuffer stores X11 wire format (BGRA).
                                    // Force alpha byte to 0xFF so GetImage returns opaque pixels
                                    // for Cairo compositing (depth 24 — padding byte must be 0xFF).
                                    for (int row = 0; row < h; row++) {
                                        uint32_t* dstRow = dstBuf + (y + row) * dW + x;
                                        const uint32_t* srcRow = src32 + row * w;
                                        for (int col = 0; col < w; col++) {
                                            dstRow[col] = srcRow[col] | 0xFF000000u;
                                        }
                                    }
                                } else if (fullyCovered && !msbFirst_) {
                                    /* Fast path with child clipping */
                                    const uint32_t* src32 = reinterpret_cast<const uint32_t*>(pixels.data());
                                    for (int row = 0; row < h; row++) {
                                        int dstY = y + row;
                                        uint32_t* dstRow = dstBuf + dstY * dW + x;
                                        const uint32_t* srcRow = src32 + row * w;
                                        for (int col = 0; col < w; col++) {
                                            int dstX = x + col;
                                            bool clipped = false;
                                            for (auto& cr : childClip) {
                                                if (dstX >= cr.x1 && dstX < cr.x2 && dstY >= cr.y1 && dstY < cr.y2) {
                                                    clipped = true; break;
                                                }
                                            }
                                            if (!clipped) dstRow[col] = srcRow[col] | 0xFF000000u;
                                        }
                                    }
                                } else {
                                    /* Slow path with bounds checking and child clipping */
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
                                            size_t srcIdx = (row * w + col) * 4;
                                            if (srcIdx + 3 >= maxPixelIdx) continue;
                                            // Store X11 wire format directly (BGRA)
                                            uint32_t pixel;
                                            if (msbFirst_) {
                                                // MSB first: [A][R][G][B] in wire → store as BGRA
                                                uint8_t a = pixels[srcIdx], r = pixels[srcIdx+1], g = pixels[srcIdx+2], b = pixels[srcIdx+3];
                                                pixel = (a << 24) | (r << 16) | (g << 8) | b;
                                            } else {
                                                // LSB first: [B][G][R][A] in wire → already BGRA, just read as uint32
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
                        {
                            std::lock_guard<std::mutex> mapLock(windowMapMutex);
                            windowSizes[wid] = {winWidth, winHeight};
                            windowPositions[wid] = {winX, winY, parentWid};
                            childWindows.push_back(wid);
                            /* In X11, newly created windows are unmapped until XMapWindow.
                             * Track this so popup windows (created at init but shown later)
                             * are invisible to hit testing until explicitly mapped. */
                            unmappedWindows.insert(wid);
                        }
                        LOGI("X11 handle CreateWindow wid=0x%x parent=0x%x pos=(%d,%d) %dx%d (childWindows size=%zu)",
                             wid, parentWid, winX, winY, winWidth, winHeight, childWindows.size());
                        // Capture the plugin's window size from the first child window
                        if (pluginWidth == 0 && winWidth > 0 && winHeight > 0) {
                            std::lock_guard<std::mutex> fbLock(bufferMutex);
                            originalChildW = winWidth;
                            originalChildH = winHeight;
                            pluginWidth = winWidth;
                            pluginHeight = winHeight;
                            // Framebuffer stores X11 wire format (BGRA): B=0x20, G=0x20, R=0x30, A=0xFF
                            uint32_t bgX11 = 0xFF302020;
                            framebuffer.assign((size_t)pluginWidth * pluginHeight, bgX11);
                            LOGI("X11: Plugin size set to %dx%d (framebuffer initial)", pluginWidth, pluginHeight);
                        }
                        /* Send Expose for the new child window so the plugin draws even if MapWindow
                         * is never sent (e.g. due to request reorder or buffering). */
                        {
                            uint16_t evtSeq = lastReplySeq_.load(std::memory_order_relaxed);
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
                            unmappedWindows.erase(wid);

                            /* Raise popup subtree to front: when a top-level window
                             * (child of root, e.g. a popup menu) is mapped, move its
                             * entire subtree to the end of childWindows so the hit test
                             * (reverse iteration) finds popup windows before regular
                             * plugin widgets.  Sort by window ID to preserve creation
                             * order within the subtree (children after parent, later
                             * siblings after earlier ones). */
                            if (!childWindows.empty() && wid != childWindows[0]) {
                                auto posIt = windowPositions.find(wid);
                                if (posIt != windowPositions.end() &&
                                    posIt->second.parent == kRootWindowId) {
                                    // BFS to collect entire subtree
                                    std::unordered_set<uint32_t> subtree;
                                    subtree.insert(wid);
                                    bool changed = true;
                                    while (changed) {
                                        changed = false;
                                        for (auto& [cw, pos] : windowPositions) {
                                            if (subtree.count(pos.parent) &&
                                                !subtree.count(cw)) {
                                                subtree.insert(cw);
                                                changed = true;
                                            }
                                        }
                                    }
                                    // Sort by window ID (= creation order)
                                    std::vector<uint32_t> toMove(subtree.begin(),
                                                                  subtree.end());
                                    std::sort(toMove.begin(), toMove.end());
                                    // Remove subtree from childWindows
                                    childWindows.erase(
                                        std::remove_if(childWindows.begin(),
                                                       childWindows.end(),
                                            [&subtree](uint32_t w) {
                                                return subtree.count(w);
                                            }),
                                        childWindows.end());
                                    // Add back at the end in creation order
                                    for (auto w : toMove)
                                        childWindows.push_back(w);
                                    LOGI("X11 MapWindow: raised subtree of 0x%x "
                                         "(%zu windows) to front", wid, toMove.size());
                                }
                            }
                        }
                        int expW = width, expH = height;
                        auto mapSizeIt = windowSizes.find(wid);
                        if (mapSizeIt != windowSizes.end()) {
                            expW = mapSizeIt->second.first;
                            expH = mapSizeIt->second.second;
                        }
                        LOGI("X11 handle MapWindow wid=0x%x -> sending Expose %dx%d", wid, expW, expH);
                        uint16_t evtSeq = lastReplySeq_.load(std::memory_order_relaxed);
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
                    case PolyFillRectangle:
                        /* No-op: skip drawing. The plugin draws all visible content via PutImage.
                         * PolyFillRectangle is typically used to clear areas before PutImage redraws,
                         * but we don't know the correct GC foreground color, so drawing with a
                         * hardcoded color causes visible darkening artifacts. Skipping is safe
                         * because PutImage overwrites the same pixels immediately after. */
                        break;
                    case GetGeometry: {
                        /* GetGeometry request: opcode(1), unused(1), length(2), drawable(4) */
                        uint32_t drawable = read32(buf, 4);
                        int geoWidth = width;
                        int geoHeight = height;
                        const char* source = "surface-default";
                        /* Check if querying a child window with stored size */
                        auto sizeIt = windowSizes.find(drawable);
                        if (sizeIt != windowSizes.end()) {
                            geoWidth = sizeIt->second.first;
                            geoHeight = sizeIt->second.second;
                            source = "windowSizes";
                        } else if (drawable == kRootWindowId && originalChildW > 0) {
                            /* Return SCALED original child window size for root/parent window queries.
                             * Plugins call XGetWindowAttributes(parentXwindow) in resize_event
                             * and resize themselves to match. Using the ORIGINAL size (not current)
                             * prevents a feedback loop where each resize shrinks the window further. */
                            geoWidth = (int)(originalChildW * uiScale);
                            geoHeight = (int)(originalChildH * uiScale);
                            if (geoWidth < 1) geoWidth = 1;
                            if (geoHeight < 1) geoHeight = 1;
                            source = "root->orig-scaled";
                        }
                        int geoX = 0, geoY = 0;
                        auto posIt = windowPositions.find(drawable);
                        if (posIt != windowPositions.end()) {
                            geoX = posIt->second.x;
                            geoY = posIt->second.y;
                        }
                        LOGI("X11 GetGeometry drawable=0x%x -> (%d,%d) %dx%d (%s)", drawable, geoX, geoY, geoWidth, geoHeight, source);
                        sendReplyGetGeometry(seq, kRootWindowId, geoX, geoY, geoWidth, geoHeight);
                        break;
                    }
                    case GetWindowAttributes: {
                        uint32_t gwaWid = read32(buf, 4);
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
                            isUnmapped = unmappedWindows.count(gwaWid) > 0;
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
                        bool isGLX = (nameLen == 3 && strncmp(extName, "GLX", 3) == 0);
                        if (reqLogCount <= 15 || isGLX)
                            LOGI("X11 handle QueryExtension '%.*s' -> %s",
                                 (int)nameLen, extName, isGLX ? "present" : "not present");
                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;  /* reply */
                        write16(reply, 2, seq);
                        if (isGLX) {
                            reply[8] = 1;  /* present */
                            reply[9] = kGLXMajorOpcode;  /* major_opcode */
                            reply[10] = 0; /* first_event */
                            reply[11] = 0; /* first_error */
                        }
                        sendReply(reply, 32, seq);
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
                        {
                            std::lock_guard<std::mutex> lock(bufferMutex);
                            const uint32_t* srcBuf = nullptr;
                            int srcW = 0, srcH = 0;
                            bool isWindow = false;
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
                                auto pit = pixmaps.find(drawable);
                                if (pit != pixmaps.end()) {
                                    srcBuf = pit->second.pixels.data();
                                    srcW = pit->second.w;
                                    srcH = pit->second.h;
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
                        PixmapData pd;
                        pd.w = pw;
                        pd.h = ph;
                        pd.pixels.resize((size_t)pw * ph, 0xFF302020);
                        pixmaps[pid] = std::move(pd);
                        break;
                    }
                    /* --- FreePixmap --- */
                    case 54: { /* FreePixmap */
                        uint32_t pid = read32(buf, 4);
                        LOGI("X11 handle FreePixmap pid=0x%x", pid);
                        pixmaps.erase(pid);
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
                            auto it = pixmaps.find(srcId);
                            if (it != pixmaps.end()) {
                                srcPixels = it->second.pixels.data();
                                sW = it->second.w; sH = it->second.h;
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
                            auto it = pixmaps.find(dstId);
                            if (it != pixmaps.end()) {
                                dstPixels = it->second.pixels.data();
                                dW = it->second.w; dH = it->second.h;
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

                        if (srcPixels && dstPixels && cw > 0 && ch > 0) {
                            for (int row = 0; row < ch; row++) {
                                int sy = srcY + row, dy = dstY + row;
                                if (sy < 0 || sy >= sH || dy < 0 || dy >= dH) continue;
                                for (int col = 0; col < cw; col++) {
                                    int sx = srcX + col, dx = dstX + col;
                                    if (sx < 0 || sx >= sW || dx < 0 || dx >= dW) continue;
                                    dstPixels[dy * dW + dx] = srcPixels[sy * sW + sx];
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

                        uint32_t atomId = 0; // None
                        auto it = atomNameToId.find(name);
                        if (it != atomNameToId.end()) {
                            atomId = it->second;
                        } else if (!onlyIfExists && !name.empty()) {
                            atomId = nextAtomId++;
                            atomNameToId[name] = atomId;
                            atomIdToName[atomId] = name;
                        }

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
                        std::string name;
                        auto it = atomIdToName.find(atomId);
                        if (it != atomIdToName.end()) {
                            name = it->second;
                        }

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
                    case GetProperty:       /* 20 */
                    case 15:  /* QueryTree */
                    case 21:  /* ListProperties */
                    case 23:  /* GetSelectionOwner */
                    case 26:  /* GrabPointer */
                    case 31:  /* GrabKeyboard */
                    case 39:  /* GetMotionEvents */
                    case 43:  /* GetInputFocus */
                    case 44:  /* QueryKeymap */
                    case 47:  /* QueryFont */
                    case 49:  /* ListFonts */
                    case 52:  /* GetFontPath */
                    case 83:  /* ListInstalledColormaps */
                    case 84:  /* AllocColor */
                    case 91:  /* QueryColors */
                    case 97:  /* QueryBestSize */
                    case 102: /* GetKeyboardMapping */
                    case 103: /* GetKeyboardControl */
                    case 104: /* GetPointerControl */
                    case 106: /* GetPointerMapping */ {
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
                        uint8_t reply[32];
                        memset(reply, 0, 32);
                        reply[0] = 1;  /* reply */
                        reply[1] = 1;  /* same-screen = true */
                        write16(reply, 2, seq);
                        write32(reply, 8, kRootWindowId);
                        write16(reply, 16, static_cast<uint16_t>(lastPointerX.load(std::memory_order_relaxed)));
                        write16(reply, 18, static_cast<uint16_t>(lastPointerY.load(std::memory_order_relaxed)));
                        write16(reply, 20, static_cast<uint16_t>(lastPointerX.load(std::memory_order_relaxed)));
                        write16(reply, 22, static_cast<uint16_t>(lastPointerY.load(std::memory_order_relaxed)));
                        uint16_t mask = 0;
                        if (pointerButton1Down.load(std::memory_order_relaxed)) mask |= (1 << 8); /* Button1Mask */
                        write16(reply, 24, mask);
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
                                if (bit == 11) {  /* CWEventMask */
                                    uint32_t eventMask = read32(buf, attrOffset);
                                    windowEventMasks[window] = eventMask;
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
                            windowEventMasks.erase(window);
                            {
                                std::lock_guard<std::mutex> mapLock(windowMapMutex);
                                windowSizes.erase(window);
                                windowPositions.erase(window);
                                unmappedWindows.erase(window);
                                childWindows.erase(
                                    std::remove(childWindows.begin(), childWindows.end(), window),
                                    childWindows.end());
                            }
                            if (reqLogCount <= 15) LOGI("X11 handle DestroyWindow window=0x%x (cleaned up)", window);
                        }
                        break;
                    case SendEvent: {
                        /* SendEvent: opcode(1), propagate(1), length(2), destination(4), event_mask(4), event(32)
                         * Body already read: buf[4..7]=destination, buf[8..11]=event_mask, buf[12..43]=event
                         * The 32-byte event at buf[12] should be forwarded to the client. */
                        if (reqLogCount <= 30) LOGI("X11 handle SendEvent (forwarding 32-byte event to client)");
                        /* The event is at buf[12..43]. Set bit 7 of byte 0 to mark as "sent via SendEvent". */
                        /* Rewrite event seq to lastReplySeq_ to match XCB expectations */
                        write16(buf + 12, 2, lastReplySeq_.load(std::memory_order_relaxed));
                        buf[12] |= 0x80;
                        sendAllLocked(clientFd, buf + 12, 32);
                        // SendEvent forwarded
                        break;
                    }
                    case 10: { /* UnmapWindow (opcode 10) */
                        uint32_t umWid = read32(buf, 4);
                        {
                            std::lock_guard<std::mutex> mapLock(windowMapMutex);
                            unmappedWindows.insert(umWid);
                        }
                        /* Send Expose to the top-level plugin window so it repaints
                           over the now-hidden window's stale pixels in the framebuffer.
                           Only do this for the popup-level window itself (not its children)
                           to avoid an event flood from recursive widget_hide. */
                        if (!childWindows.empty() && umWid != childWindows[0]) {
                            /* Check if this is a top-level popup (parent is root) */
                            auto posIt = windowPositions.find(umWid);
                            if (posIt != windowPositions.end() &&
                                posIt->second.parent == kRootWindowId) {
                                uint32_t topWin = childWindows[0];
                                auto szIt = windowSizes.find(topWin);
                                if (szIt != windowSizes.end()) {
                                    uint16_t evtSeq = lastReplySeq_.load(std::memory_order_relaxed);
                                    uint8_t evt[32];
                                    memset(evt, 0, 32);
                                    evt[0] = Expose;
                                    write16(evt, 2, evtSeq);
                                    write32(evt, 4, topWin);
                                    write16(evt, 8, 0);
                                    write16(evt, 10, 0);
                                    write16(evt, 12, (uint16_t)szIt->second.first);
                                    write16(evt, 14, (uint16_t)szIt->second.second);
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
                        {
                            std::lock_guard<std::mutex> mapLock(windowMapMutex);
                            if (vmask & 0x0001) {
                                int newX = (int)(int32_t)read32(buf, valOff); valOff += 4;
                                auto posIt = windowPositions.find(cfgWid);
                                if (posIt != windowPositions.end()) {
                                    if (posIt->second.x != newX) {
                                        posIt->second.x = newX;
                                        posChanged = true;
                                    }
                                }
                            }
                            if (vmask & 0x0002) {
                                int newY = (int)(int32_t)read32(buf, valOff); valOff += 4;
                                auto posIt = windowPositions.find(cfgWid);
                                if (posIt != windowPositions.end()) {
                                    if (posIt->second.y != newY) {
                                        posIt->second.y = newY;
                                        posChanged = true;
                                    }
                                }
                            }
                            /* Capture top-level window info for Expose if position changed */
                            if (posChanged && !childWindows.empty()) {
                                exposeTopWid = childWindows[0];
                                auto sizeIt = windowSizes.find(exposeTopWid);
                                if (sizeIt != windowSizes.end()) {
                                    exposeTopW = sizeIt->second.first;
                                    exposeTopH = sizeIt->second.second;
                                }
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
                            for (auto wid : childWindows) { if (wid == cfgWid) { isChildWin = true; break; } }

                            if (isChildWin && (newW > 0 || newH > 0)) {
                                /* Compute final dimensions */
                                auto& stored = windowSizes[cfgWid];
                                finalW = (newW > 0) ? newW : stored.first;
                                finalH = (newH > 0) ? newH : stored.second;

                                /* Only act on actual size changes to avoid flicker from repeated
                                 * same-size ConfigureWindow calls (plugins often send these every idle). */
                                sizeChanged = (cfgWid == childWindows[0])
                                    ? (finalW != pluginWidth || finalH != pluginHeight)
                                    : (finalW != stored.first || finalH != stored.second);

                                stored.first  = finalW;
                                stored.second = finalH;
                            }
                        }

                        if (isChildWin && (newW > 0 || newH > 0)) {
                            if (sizeChanged) {
                                LOGI("X11 ConfigureWindow wid=0x%x size %dx%d (vmask=0x%04x)", cfgWid, finalW, finalH, (unsigned)vmask);
                            }

                            /* If this is the tracked plugin window and the size changed,
                             * resize the framebuffer — preserve existing pixels (no dark wipe). */
                            bool isTopLevel = false;
                            {
                                std::lock_guard<std::mutex> mapLock(windowMapMutex);
                                isTopLevel = !childWindows.empty() && cfgWid == childWindows[0];
                            }
                            if (sizeChanged && isTopLevel) {
                                std::lock_guard<std::mutex> fbLock(bufferMutex);
                                pluginWidth  = finalW;
                                pluginHeight = finalH;
                                uint32_t bgX11 = 0xFF302020;
                                /* resize() preserves existing pixels; only fills newly added pixels */
                                framebuffer.resize((size_t)pluginWidth * pluginHeight, bgX11);
                            }

                            /* Only send ConfigureNotify + Expose on actual size changes.
                             * Sending ConfigureNotify on same-size XResizeWindow creates a
                             * feedback loop: ConfigureNotify → resize_event → XResizeWindow
                             * → ConfigureNotify → ..., causing rendering artifacts. */
                            /* Send ConfigureNotify + Expose only on actual size changes */
                            if (sizeChanged) {
                                uint16_t evtSeq = lastReplySeq_.load(std::memory_order_relaxed);
                                uint8_t cfgNotify[32];
                                memset(cfgNotify, 0, 32);
                                cfgNotify[0] = 22;  /* ConfigureNotify */
                                write16(cfgNotify, 2, evtSeq);
                                write32(cfgNotify, 4, cfgWid);  /* event window */
                                write32(cfgNotify, 8, cfgWid);  /* window */
                                write32(cfgNotify, 12, 0);       /* above-sibling: None */
                                write16(cfgNotify, 16, 0);       /* x */
                                write16(cfgNotify, 18, 0);       /* y */
                                write16(cfgNotify, 20, (uint16_t)finalW);
                                write16(cfgNotify, 22, (uint16_t)finalH);
                                write16(cfgNotify, 24, 0);       /* border-width */
                                cfgNotify[26] = 0;               /* override-redirect */
                                sendAllLocked(clientFd, cfgNotify, 32);

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
                            if (reqLogCount <= 20) LOGI("X11 ConfigureWindow wid=0x%x vmask=0x%04x (no size change)", cfgWid, (unsigned)vmask);
                        }

                        /* If a child window's position changed (XMoveWindow), send Expose
                         * for the top-level plugin window so the plugin redraws everything
                         * at the new positions.  Unlike a real X server, we can't relocate
                         * pixels in our single-framebuffer architecture — the client must
                         * redraw.  Multiple Expose events for the same window are coalesced
                         * by the client's event loop (XCheckTypedWindowEvent). */
                        if (posChanged && exposeTopWid && exposeTopW > 0 && exposeTopH > 0) {
                            uint16_t evtSeq = lastReplySeq_.load(std::memory_order_relaxed);
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
                        /* GLX extension requests.
                         * Mesa's xlib GLX with swrast does most rendering client-side.
                         * The server only needs to handle a few GLX queries.
                         * GLX sub-opcode is in buf[1] (the "data" byte of the request header). */
                        uint8_t glxMinor = buf[1];
                        /* GLX sub-opcodes:
                         *   1  = glXRender (void, no reply)
                         *   2  = glXRenderLarge (void, no reply)
                         *   3  = glXCreateContext (void, no reply)
                         *   4  = glXDestroyContext (void, no reply)
                         *   5  = glXMakeCurrent (reply)
                         *   6  = glXIsDirect (reply)
                         *   7  = glXQueryVersion (reply)
                         *   8  = glXWaitGL (void)
                         *   9  = glXWaitX (void)
                         *  10  = glXCopyContext (void)
                         *  11  = glXSwapBuffers (void)
                         *  14  = glXGetVisualConfigs (reply)
                         *  17  = glXQueryServerString (reply)
                         *  18  = glXClientInfo (void)
                         *  19  = glXGetFBConfigs (reply)
                         *  20  = glXCreatePixmap (void)
                         *  21  = glXDestroyPixmap (void)
                         *  22  = glXCreateNewContext (void)
                         *  24  = glXMakeContextCurrent (reply)
                         *  26  = glXQueryContext (reply)
                         */
                        LOGI("X11 handle GLX sub-opcode=%u length=%u", (unsigned)glxMinor, (unsigned)length);
                        switch (glxMinor) {
                            case 7: { /* glXQueryVersion */
                                uint8_t reply[32];
                                memset(reply, 0, 32);
                                reply[0] = 1;  /* reply */
                                write16(reply, 2, seq);
                                write32(reply, 8, 1);   /* major version = 1 */
                                write32(reply, 12, 4);  /* minor version = 4 */
                                sendReply(reply, 32, seq);
                                break;
                            }
                            case 5: { /* glXMakeCurrent */
                                /* Reply: context tag (32-bit) */
                                uint8_t reply[32];
                                memset(reply, 0, 32);
                                reply[0] = 1;
                                write16(reply, 2, seq);
                                write32(reply, 8, 1);  /* context_tag = 1 (non-zero = success) */
                                sendReply(reply, 32, seq);
                                break;
                            }
                            case 24: { /* glXMakeContextCurrent */
                                uint8_t reply[32];
                                memset(reply, 0, 32);
                                reply[0] = 1;
                                write16(reply, 2, seq);
                                write32(reply, 8, 1);  /* context_tag */
                                sendReply(reply, 32, seq);
                                break;
                            }
                            case 6: { /* glXIsDirect */
                                uint8_t reply[32];
                                memset(reply, 0, 32);
                                reply[0] = 1;
                                write16(reply, 2, seq);
                                reply[8] = 0;  /* is_direct = false (indirect rendering) */
                                sendReply(reply, 32, seq);
                                break;
                            }
                            case 14: { /* glXGetVisualConfigs */
                                /* Return a single visual config matching our 32-bit ARGB visual.
                                 * Each config is 28 uint32_t property pairs (GLX_VISUAL_ID, ...).
                                 * Mesa's xlib GLX often bypasses server configs and uses client-side
                                 * visual matching, but we provide one config for completeness. */
                                const uint32_t numConfigs = 1;
                                const uint32_t numProps = 28;
                                size_t dataBytes = numConfigs * numProps * 4;
                                size_t padded = (dataBytes + 3) & ~3u;
                                size_t replySize = 32 + padded;
                                std::vector<uint8_t> reply(replySize, 0);
                                reply[0] = 1;
                                write16(reply.data(), 2, seq);
                                write32(reply.data(), 4, (uint32_t)(padded / 4));
                                write32(reply.data(), 8, numConfigs);
                                write32(reply.data(), 12, numProps);
                                /* Fill config properties: visual_id(0), class(TrueColor=4),
                                 * rgba bits, buffer size, depth, stencil, accum, etc. */
                                uint32_t* props = reinterpret_cast<uint32_t*>(reply.data() + 32);
                                props[0]  = kDefaultVisualId;  /* visual ID */
                                props[1]  = 4;     /* class: TrueColor */
                                props[2]  = 1;     /* RGBA: true */
                                props[3]  = 8;     /* red bits */
                                props[4]  = 8;     /* green bits */
                                props[5]  = 8;     /* blue bits */
                                props[6]  = 8;     /* alpha bits */
                                props[7]  = 0;     /* accum red */
                                props[8]  = 0;     /* accum green */
                                props[9]  = 0;     /* accum blue */
                                props[10] = 0;     /* accum alpha */
                                props[11] = 1;     /* double buffer */
                                props[12] = 0;     /* stereo */
                                props[13] = 32;    /* buffer size */
                                props[14] = 24;    /* depth size */
                                props[15] = 8;     /* stencil size */
                                props[16] = 0;     /* aux buffers */
                                props[17] = 0;     /* level */
                                /* remaining props stay 0 */
                                sendReply(reply.data(), replySize, seq);
                                break;
                            }
                            case 19: { /* glXGetFBConfigs */
                                /* Similar to GetVisualConfigs but for fbconfigs.
                                 * Return a single fbconfig with reasonable defaults. */
                                const uint32_t numConfigs = 1;
                                const uint32_t numAttribs = 28;
                                size_t dataBytes = numConfigs * numAttribs * 2 * 4; /* key-value pairs */
                                size_t padded = (dataBytes + 3) & ~3u;
                                size_t replySize = 32 + padded;
                                std::vector<uint8_t> reply(replySize, 0);
                                reply[0] = 1;
                                write16(reply.data(), 2, seq);
                                write32(reply.data(), 4, (uint32_t)(padded / 4));
                                write32(reply.data(), 8, numConfigs);
                                write32(reply.data(), 12, numAttribs);
                                /* FBConfig attribs as key-value pairs: GLX_FBCONFIG_ID, value, ... */
                                uint32_t* kv = reinterpret_cast<uint32_t*>(reply.data() + 32);
                                int idx = 0;
                                auto addAttr = [&](uint32_t key, uint32_t val) {
                                    kv[idx++] = key; kv[idx++] = val;
                                };
                                addAttr(0x8013, 1);    /* GLX_FBCONFIG_ID = 1 */
                                addAttr(0x8010, 32);   /* GLX_BUFFER_SIZE = 32 */
                                addAttr(0x8011, 0);    /* GLX_LEVEL = 0 */
                                addAttr(0x8012, 1);    /* GLX_DOUBLEBUFFER = 1 */
                                addAttr(0x8014, 4);    /* GLX_VISUAL_TYPE = GLX_TRUE_COLOR */
                                addAttr(0x8015, 8);    /* GLX_RED_SIZE = 8 */
                                addAttr(0x8016, 8);    /* GLX_GREEN_SIZE = 8 */
                                addAttr(0x8017, 8);    /* GLX_BLUE_SIZE = 8 */
                                addAttr(0x8018, 8);    /* GLX_ALPHA_SIZE = 8 */
                                addAttr(0x8019, 24);   /* GLX_DEPTH_SIZE = 24 */
                                addAttr(0x801A, 8);    /* GLX_STENCIL_SIZE = 8 */
                                addAttr(0x8020, 0x8002); /* GLX_RENDER_TYPE = GLX_RGBA_BIT */
                                addAttr(0x8021, 0x8001); /* GLX_DRAWABLE_TYPE = GLX_WINDOW_BIT */
                                addAttr(0x8022, 0);    /* GLX_X_RENDERABLE = 0 */
                                addAttr(0x8023, 0);    /* GLX_X_VISUAL_TYPE = 0 */
                                addAttr(0x20,   0);    /* GLX_NONE (terminator for remaining) */
                                /* Fill remaining with zeros */
                                sendReply(reply.data(), replySize, seq);
                                break;
                            }
                            case 17: { /* glXQueryServerString */
                                /* Return an empty string. The reply is:
                                 * 32-byte header + string length (4 bytes) + string data (padded) */
                                uint8_t reply[36];
                                memset(reply, 0, 36);
                                reply[0] = 1;
                                write16(reply, 2, seq);
                                write32(reply, 4, 1);  /* reply length = 1 (4 bytes of data) */
                                write32(reply, 8, 0);  /* string length = 0 */
                                sendReply(reply, 36, seq);
                                break;
                            }
                            case 26: { /* glXQueryContext */
                                /* Return empty context attribs */
                                uint8_t reply[32];
                                memset(reply, 0, 32);
                                reply[0] = 1;
                                write16(reply, 2, seq);
                                sendReply(reply, 32, seq);
                                break;
                            }
                            /* Void GLX requests (no reply expected) */
                            case 1:  /* glXRender */
                            case 2:  /* glXRenderLarge */
                            case 3:  /* glXCreateContext */
                            case 4:  /* glXDestroyContext */
                            case 8:  /* glXWaitGL */
                            case 9:  /* glXWaitX */
                            case 10: /* glXCopyContext */
                            case 11: /* glXSwapBuffers */
                            case 18: /* glXClientInfo */
                            case 20: /* glXCreatePixmap */
                            case 21: /* glXDestroyPixmap */
                            case 22: /* glXCreateNewContext */
                                break;
                            default: {
                                /* Unknown GLX sub-opcode — send generic empty reply to avoid desync.
                                 * GLX requests with reply bit set will block if we don't reply. */
                                LOGI("X11 GLX unhandled sub-opcode=%u (sending generic reply)", (unsigned)glxMinor);
                                uint8_t reply[32];
                                memset(reply, 0, 32);
                                reply[0] = 1;
                                write16(reply, 2, seq);
                                sendReply(reply, 32, seq);
                                break;
                            }
                        }
                        break;
                    }
                    case ChangeProperty:
                    case DeleteProperty:
                    case 22: /* SetSelectionOwner (void) */
                    case 24: /* ConvertSelection (void) */
                    case 42: /* SetInputFocus */
                    case 51: /* SetFontPath */
                    case 55: /* CreateGC */
                    case 56: /* ChangeGC */
                    case 57: /* CopyGC */
                    case 58: /* SetDashes */
                    case 59: /* SetClipRectangles */
                    case 60: /* FreeGC */
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
                    case 101: /* ChangeKeyboardMapping */
                        /* first-seen already logged at opcode entry above */
                        break;
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
                close(clientFd);
                clientFd = -1;
            }
        }
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

bool X11NativeDisplay::attachSurface(JNIEnv* jniEnv, jobject jSurface, int width, int height) {
    if (!jniEnv || !jSurface) return false;

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
    // Framebuffer stores X11 wire format (BGRA) directly
    impl_->framebuffer.assign((size_t)impl_->width * impl_->height, 0xFF302020);
    impl_->dirty = true;
    // Note: render thread not started yet, no need to notify

    impl_->running = true;
    impl_->renderThreadRunning = true;
    impl_->listening_ = false;
    impl_->pluginUIRunning = true;
    impl_->renderThreadExited.store(false, std::memory_order_release);
    impl_->serverThread = std::thread(&Impl::serverLoop, impl_.get());
    impl_->renderThread = std::thread(&Impl::renderLoop, impl_.get());
    impl_->pluginUIThread = std::thread(&Impl::pluginUILoop, impl_.get());

    for (int i = 0; i < 100 && !impl_->listening_; i++) {
        usleep(10000);
    }
    if (!impl_->listening_) {
        LOGI("Display %d: server not listening after 1s (continuing anyway)", displayNumber_);
    }

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
    
    // Keep running=true so server thread can process graceful teardown
    // The server loop will check closingGracefully and send DestroyNotify
    
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
        if (impl_->window)
            ANativeWindow_setBuffersGeometry(impl_->window, width, height, 1);
    }
}

void X11NativeDisplay::injectTouch(int action, int x, int y) {
    static int touchLogCount = 0;
    const int n = ++touchLogCount;
    const char* actionName = (action == 0) ? "DOWN" : (action == 1) ? "UP" : "MOVE";
    // Map from Android surface coordinates to plugin's natural coordinate space (accounts for letterbox)
    if (impl_->pluginWidth > 0 && impl_->width > 0 && impl_->height > 0) {
        float scaleX = (float)impl_->width / impl_->pluginWidth;
        float scaleY = (float)impl_->height / impl_->pluginHeight;
        float scale = scaleX < scaleY ? scaleX : scaleY;
        int x0 = (impl_->width  - (int)(impl_->pluginWidth  * scale)) / 2;
        int y0 = (impl_->height - (int)(impl_->pluginHeight * scale)) / 2;
        x = (int)((x - x0) / scale);
        y = (int)((y - y0) / scale);
    }
    if (n <= 50 || n % 30 == 0) {
        LOGI("injectTouch: display=%d action=%s (%d) plugin=(%d,%d) [call #%d]", displayNumber_, actionName, action, x, y, n);
    }
    if (impl_->clientFd < 0) {
        if (n <= 10 || n % 50 == 0) LOGI("injectTouch: clientFd < 0, ignoring (display=%d)", displayNumber_);
        return;
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

void withDisplaySetSurfaceSize(int displayNumber, int width, int height) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    if (it != g_displays.end())
        it->second->setSurfaceSize(width, height);
}

bool withDisplayGetPluginSize(int displayNumber, int& w, int& h) {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    auto it = g_displays.find(displayNumber);
    if (it != g_displays.end())
        return it->second->getPluginSize(w, h);
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
