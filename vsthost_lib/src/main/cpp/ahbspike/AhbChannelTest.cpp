// AhbChannelTest.cpp — see AhbChannelTest.h. Synthetic Phase 2 producer.

#include "AhbChannelTest.h"
#include "../x11/AhbChannelProtocol.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace guitarrackcraft {

namespace {

FILE* g_log = nullptr;
void tlog(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "AhbChannelTest", "%s", buf);
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
}

// Fill an AHB with a gradient in B,G,R,A byte order (matches the X server's
// framebuffer wire order + its BGRA->RGBA shader swizzle). DISTINCT from the
// Phase 1 direct hook: here red ramps across x, BLUE ramps down y, green=64 —
// so on-screen you can tell a socket-delivered frame from a direct one.
bool fillGradient(AHardwareBuffer* ahb, int w, int h) {
    AHardwareBuffer_Desc actual = {};
    AHardwareBuffer_describe(ahb, &actual);
    void* ptr = nullptr;
    if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                             -1, nullptr, &ptr) != 0 || !ptr) {
        tlog("fillGradient: lock failed");
        return false;
    }
    const uint32_t stride = actual.stride ? actual.stride : (uint32_t)w;
    uint8_t* base = (uint8_t*)ptr;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = base + (size_t)y * stride * 4;
        uint8_t bch = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));   // blue ramps down rows
        for (int x = 0; x < w; ++x) {
            uint8_t rch = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1)); // red ramps across cols
            uint8_t* px = row + (size_t)x * 4;
            px[0] = bch;  // B
            px[1] = 64;   // G
            px[2] = rch;  // R
            px[3] = 255;  // A
        }
    }
    AHardwareBuffer_unlock(ahb, nullptr);
    return true;
}

void sleepMs(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

} // namespace

bool runAhbChannelTest(int displayNumber, int holdMs, const char* logPath) {
    g_log = logPath ? fopen(logPath, "w") : nullptr;
    tlog("=== AhbChannelTest start display=%d holdMs=%d ===", displayNumber, holdMs);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { tlog("socket failed: %s", strerror(errno)); if (g_log) fclose(g_log); return false; }

    struct sockaddr_un addr;
    socklen_t alen = ahbch_make_addr(&addr, displayNumber);
    if (connect(fd, (struct sockaddr*)&addr, alen) < 0) {
        tlog("connect to 'guitarrack-ahb-%d' failed: %s (is the editor/server up?)",
             displayNumber, strerror(errno));
        close(fd);
        if (g_log) fclose(g_log);
        return false;
    }
    tlog("connected to abstract 'guitarrack-ahb-%d'", displayNumber);

    const int W = 512, H = 512;
    AHardwareBuffer_Desc desc = {};
    desc.width  = (uint32_t)W;
    desc.height = (uint32_t)H;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage  = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                  AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    AHardwareBuffer* ahb = nullptr;
    if (AHardwareBuffer_allocate(&desc, &ahb) != 0 || !ahb) {
        tlog("AHardwareBuffer_allocate failed");
        close(fd);
        if (g_log) fclose(g_log);
        return false;
    }
    if (!fillGradient(ahb, W, H)) {
        AHardwareBuffer_release(ahb);
        close(fd);
        if (g_log) fclose(g_log);
        return false;
    }
    tlog("allocated + filled %dx%d gradient AHB", W, H);

    const uint32_t kWindowId = 0xCAFE;  // server routes any wid to the editor slot

    AhbChMsg reg = {};
    reg.magic = AHBCH_MAGIC; reg.version = AHBCH_VERSION; reg.type = AHBCH_REGISTER;
    reg.window_id = kWindowId; reg.width = W; reg.height = H;
    reg.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM; reg.buffer_count = 1;
    if (!ahbch_send_msg(fd, &reg, -1)) { tlog("send REGISTER failed"); goto fail; }
    if (AHardwareBuffer_sendHandleToUnixSocket(ahb, fd) != 0) {
        tlog("AHardwareBuffer_sendHandleToUnixSocket failed"); goto fail;
    }
    tlog("sent REGISTER + 1 AHB handle");

    {
        AhbChMsg pres = {};
        pres.magic = AHBCH_MAGIC; pres.version = AHBCH_VERSION; pres.type = AHBCH_PRESENT;
        pres.window_id = kWindowId; pres.buffer_index = 0; pres.has_fence = 0;
        if (!ahbch_send_msg(fd, &pres, -1)) { tlog("send PRESENT failed"); goto fail; }
    }
    tlog("sent PRESENT idx=0 (gradient should now show on the editor) — holding %dms", holdMs);

    sleepMs(holdMs);

    {
        AhbChMsg unreg = {};
        unreg.magic = AHBCH_MAGIC; unreg.version = AHBCH_VERSION; unreg.type = AHBCH_UNREGISTER;
        unreg.window_id = kWindowId;
        ahbch_send_msg(fd, &unreg, -1);
    }
    tlog("sent UNREGISTER — editor should revert to CPU framebuffer");

    AHardwareBuffer_release(ahb);
    close(fd);
    tlog("=== AhbChannelTest PASS ===");
    if (g_log) fclose(g_log);
    return true;

fail:
    AHardwareBuffer_release(ahb);
    close(fd);
    tlog("=== AhbChannelTest FAIL ===");
    if (g_log) fclose(g_log);
    return false;
}

} // namespace guitarrackcraft
