#pragma once

#include "X11ByteOrder.h"
#include "X11Protocol.h"
#include "X11AtomStore.h"
#include "X11WindowManager.h"
#include "X11PixmapStore.h"
#include "X11Framebuffer.h"
#include "X11ConnectionHandler.h"
#include "X11EventBuilder.h"
#include "X11PropertyStore.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <poll.h>

namespace guitarrackcraft {
namespace test {

// Minimal X11 test server that handles protocol dispatch over a socketpair or TCP.
// Uses all extracted modules but no Android dependencies.
class X11TestServer {
public:
    X11TestServer() : windowManager_(kRootWindowId) {}

    ~X11TestServer() { stop(); }

    // Start the server over a socketpair. Returns the client-side fd.
    int start(int displayWidth = 800, int displayHeight = 600) {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) return -1;
        serverFd_ = fds[0];
        clientFd_ = fds[1];
        displayWidth_ = displayWidth;
        displayHeight_ = displayHeight;
        framebuffer_.resize(displayWidth, displayHeight);
        running_ = true;
        serverThread_ = std::thread([this] { serverLoop(); });
        return clientFd_;
    }

    // Start the server listening on TCP port 6000+displayNum. Returns the port.
    int startTCP(int displayNum, int displayWidth = 800, int displayHeight = 600) {
        displayWidth_ = displayWidth;
        displayHeight_ = displayHeight;
        framebuffer_.resize(displayWidth, displayHeight);
        tcpDisplayNum_ = displayNum;
        int port = 6000 + displayNum;

        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) return -1;
        int opt = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (bind(listenFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(listenFd_); listenFd_ = -1; return -1;
        }
        if (listen(listenFd_, 1) < 0) {
            close(listenFd_); listenFd_ = -1; return -1;
        }

        running_ = true;
        serverThread_ = std::thread([this] {
            struct pollfd pfd = {listenFd_, POLLIN, 0};
            int ret = poll(&pfd, 1, 5000);
            if (ret <= 0 || !running_) return;
            serverFd_ = accept(listenFd_, nullptr, nullptr);
            if (serverFd_ < 0) return;
            int flag = 1;
            setsockopt(serverFd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
            serverLoop();
        });
        return port;
    }

    int displayNum() const { return tcpDisplayNum_; }

    void stop() {
        running_ = false;
        if (listenFd_ >= 0) { close(listenFd_); listenFd_ = -1; }
        if (serverFd_ >= 0) { shutdown(serverFd_, SHUT_RDWR); close(serverFd_); serverFd_ = -1; }
        if (serverThread_.joinable()) serverThread_.join();
    }

private:
    bool recvAll(int fd, void* buf, size_t len) {
        uint8_t* p = (uint8_t*)buf;
        while (len > 0) {
            ssize_t n = recv(fd, p, len, 0);
            if (n <= 0) return false;
            p += n; len -= n;
        }
        return true;
    }

    bool sendAll(int fd, const void* buf, size_t len) {
        const uint8_t* p = (const uint8_t*)buf;
        while (len > 0) {
            ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
            if (n <= 0) return false;
            p += n; len -= n;
        }
        return true;
    }

    void sendError(uint8_t code, uint16_t seq, uint32_t badValue, uint8_t majorOp = 0) {
        uint8_t buf[32] = {};
        buf[0] = 0;  // Error indicator
        buf[1] = code;
        byteOrder_.write16(buf, 2, seq);
        byteOrder_.write32(buf, 4, badValue);
        buf[10] = majorOp;
        sendAll(serverFd_, buf, 32);
    }

    void serverLoop() {
        // 1) Handshake
        uint8_t req[12];
        if (!recvAll(serverFd_, req, 12)) return;
        auto hs = X11ConnectionHandler::parseConnectionRequest(req);
        if (!hs.success) return;
        byteOrder_.msbFirst = hs.msbFirst;
        eventBuilder_ = X11EventBuilder(byteOrder_);

        // Read and discard auth data
        if (hs.authNameLen > 0) {
            uint16_t padded = hs.authNameLen + ((4 - (hs.authNameLen % 4)) % 4);
            std::vector<uint8_t> skip(padded);
            recvAll(serverFd_, skip.data(), padded);
        }
        if (hs.authDataLen > 0) {
            uint16_t padded = hs.authDataLen + ((4 - (hs.authDataLen % 4)) % 4);
            std::vector<uint8_t> skip(padded);
            recvAll(serverFd_, skip.data(), padded);
        }

        // Send connection reply
        auto reply = X11ConnectionHandler::buildConnectionReply(byteOrder_, displayWidth_, displayHeight_);
        if (!sendAll(serverFd_, reply.data(), reply.size())) return;

        // 2) Request loop
        uint16_t seq = 0;
        while (running_) {
            struct pollfd pfd = {serverFd_, POLLIN, 0};
            int ret = poll(&pfd, 1, 50);
            if (ret <= 0) continue;
            if (pfd.revents & (POLLERR | POLLHUP)) break;

            uint8_t buf[65536];
            if (!recvAll(serverFd_, buf, 4)) break;
            uint8_t opcode = buf[0];
            uint16_t length = byteOrder_.read16(buf, 2);
            seq++;

            // BigRequests: length=0 means 32-bit extended length follows
            if (length == 0) {
                uint8_t extBuf[4];
                if (!recvAll(serverFd_, extBuf, 4)) break;
                uint32_t bigLength = byteOrder_.read32(extBuf, 0);
                if (bigLength > 2) {
                    size_t skipBytes = ((size_t)bigLength - 2) * 4;
                    while (skipBytes > 0) {
                        uint8_t tmp[4096];
                        size_t chunk = std::min(skipBytes, sizeof(tmp));
                        if (!recvAll(serverFd_, tmp, chunk)) break;
                        skipBytes -= chunk;
                    }
                }
                continue;
            }

            // Read remaining body
            size_t extra = 0;
            if (length > 1) {
                extra = (length - 1) * 4;
                if (extra > sizeof(buf) - 4) { break; }
                if (!recvAll(serverFd_, buf + 4, extra)) break;
            }

            handleRequest(opcode, buf, length, seq);
        }
    }

    // Check if a drawable is a known window (root or child)
    bool isWindow(uint32_t drawable) const {
        if (drawable == kRootWindowId) return true;
        for (auto wid : windowManager_.childWindows()) {
            if (wid == drawable) return true;
        }
        return false;
    }

    // Resolve a drawable to a pixel buffer and dimensions
    struct DrawableInfo { uint32_t* pixels; int w; int h; };
    DrawableInfo resolveDrawable(uint32_t drawable) {
        if (isWindow(drawable) && !framebuffer_.empty()) {
            return {framebuffer_.data(), framebuffer_.width(), framebuffer_.height()};
        }
        auto* pm = pixmapStore_.get(drawable);
        if (pm) return {pm->pixels.data(), pm->w, pm->h};
        return {nullptr, 0, 0};
    }

    void handleRequest(uint8_t opcode, uint8_t* buf, uint16_t length, uint16_t seq) {
        using namespace X11Op;

        switch (opcode) {
            case CreateWindow: {
                uint32_t wid = byteOrder_.read32(buf, 4);
                uint32_t parent = byteOrder_.read32(buf, 8);
                int16_t x = (int16_t)byteOrder_.read16(buf, 12);
                int16_t y = (int16_t)byteOrder_.read16(buf, 14);
                uint16_t w = byteOrder_.read16(buf, 16);
                uint16_t h = byteOrder_.read16(buf, 18);
                windowManager_.createWindow(wid, parent, x, y, w, h);
                auto evt = eventBuilder_.expose(wid, 0, 0, w, h, 0, seq);
                sendAll(serverFd_, evt.data(), 32);
                break;
            }
            case MapWindow: {
                uint32_t wid = byteOrder_.read32(buf, 4);
                windowManager_.mapWindow(wid);
                auto sz = windowManager_.getSize(wid);
                auto evt = eventBuilder_.expose(wid, 0, 0, sz.first, sz.second, 0, seq);
                sendAll(serverFd_, evt.data(), 32);
                break;
            }
            case UnmapWindow: {
                uint32_t wid = byteOrder_.read32(buf, 4);
                windowManager_.unmapWindow(wid);
                break;
            }
            case DestroyWindow: {
                uint32_t wid = byteOrder_.read32(buf, 4);
                windowManager_.destroyWindow(wid);
                break;
            }
            case GetWindowAttributes: {
                uint32_t wid = byteOrder_.read32(buf, 4);
                uint8_t reply[44] = {};
                reply[0] = 1;       // Reply
                reply[1] = 0;       // backing-store
                byteOrder_.write16(reply, 2, seq);
                byteOrder_.write32(reply, 4, 3);  // reply length (44-32)/4 = 3
                byteOrder_.write32(reply, 8, kDefaultVisualId);  // visual
                byteOrder_.write16(reply, 12, 1);  // class = InputOutput
                // 14: bit-gravity, 15: win-gravity (0)
                // 16-19: backing-planes, 20-23: backing-pixel (0)
                // 24: save-under, 25: map-is-installed
                reply[26] = windowManager_.isMapped(wid) ? 2 : 0;  // map-state
                // 27: override-redirect
                byteOrder_.write32(reply, 28, kDefaultColormapId);  // colormap
                sendAll(serverFd_, reply, 44);
                break;
            }
            case GetGeometry: {
                uint32_t drawable = byteOrder_.read32(buf, 4);
                // Check if drawable exists
                if (!isWindow(drawable) && !pixmapStore_.exists(drawable)) {
                    sendError(9 /*BadDrawable*/, seq, drawable, opcode);
                    break;
                }
                auto pos = windowManager_.getPosition(drawable);
                auto sz = windowManager_.getSize(drawable);
                // For pixmaps, use pixmap dimensions
                if (!isWindow(drawable)) {
                    auto* pm = pixmapStore_.get(drawable);
                    if (pm) { sz = {pm->w, pm->h}; pos = {0, 0, 0}; }
                }
                uint8_t reply[32] = {};
                reply[0] = 1;
                reply[1] = 24;
                byteOrder_.write16(reply, 2, seq);
                byteOrder_.write32(reply, 8, kRootWindowId);
                byteOrder_.write16(reply, 12, (uint16_t)(int16_t)pos.x);
                byteOrder_.write16(reply, 14, (uint16_t)(int16_t)pos.y);
                byteOrder_.write16(reply, 16, (uint16_t)sz.first);
                byteOrder_.write16(reply, 18, (uint16_t)sz.second);
                sendAll(serverFd_, reply, 32);
                break;
            }
            case InternAtom: {
                uint8_t onlyIfExists = buf[1];
                uint16_t nameLen = byteOrder_.read16(buf, 4);
                std::string name;
                if (nameLen > 0 && nameLen <= 256)
                    name.assign(reinterpret_cast<const char*>(buf + 8), nameLen);
                uint32_t atomId = atoms_.intern(name, onlyIfExists);
                uint8_t reply[32] = {};
                reply[0] = 1;
                byteOrder_.write16(reply, 2, seq);
                byteOrder_.write32(reply, 8, atomId);
                sendAll(serverFd_, reply, 32);
                break;
            }
            case GetAtomName: {
                uint32_t atomId = byteOrder_.read32(buf, 4);
                std::string name = atoms_.getName(atomId);
                uint16_t nameLen = (uint16_t)name.size();
                uint32_t pad = (4 - (nameLen % 4)) % 4;
                uint32_t replyLen = (nameLen + pad) / 4;
                uint32_t replySize = 32 + nameLen + pad;
                std::vector<uint8_t> reply(replySize, 0);
                reply[0] = 1;
                byteOrder_.write16(reply.data(), 2, seq);
                byteOrder_.write32(reply.data(), 4, replyLen);
                byteOrder_.write16(reply.data(), 8, nameLen);
                if (nameLen > 0) memcpy(reply.data() + 32, name.data(), nameLen);
                sendAll(serverFd_, reply.data(), replySize);
                break;
            }
            case QueryPointer: {
                uint8_t reply[32] = {};
                reply[0] = 1;
                reply[1] = 1; // same-screen
                byteOrder_.write16(reply, 2, seq);
                byteOrder_.write32(reply, 8, kRootWindowId);
                sendAll(serverFd_, reply, 32);
                break;
            }
            case QueryExtension: {
                uint16_t nameLen = byteOrder_.read16(buf, 4);
                std::string name(reinterpret_cast<const char*>(buf + 8), nameLen);
                uint8_t reply[32] = {};
                reply[0] = 1;
                byteOrder_.write16(reply, 2, seq);
                if (name == "GLX") {
                    reply[8] = 1;
                    reply[9] = X11Op::kGLXMajorOpcode;
                }
                sendAll(serverFd_, reply, 32);
                break;
            }
            case ListExtensions: {
                const char* ext = "GLX";
                uint8_t nameLen = 3;
                uint32_t dataLen = 1 + nameLen;
                uint32_t pad = (4 - (dataLen % 4)) % 4;
                uint32_t replySize = 32 + dataLen + pad;
                std::vector<uint8_t> reply(replySize, 0);
                reply[0] = 1;
                reply[1] = 1;
                byteOrder_.write16(reply.data(), 2, seq);
                byteOrder_.write32(reply.data(), 4, (dataLen + pad) / 4);
                reply[32] = nameLen;
                memcpy(reply.data() + 33, ext, nameLen);
                sendAll(serverFd_, reply.data(), replySize);
                break;
            }
            // --- Property operations ---
            case ChangeProperty: {
                // Request: opcode(1), mode(1), length(2), window(4), property(4), type(4),
                //          format(1), pad(3), data_length(4), data(...)
                uint8_t mode = buf[1];
                uint32_t window = byteOrder_.read32(buf, 4);
                uint32_t property = byteOrder_.read32(buf, 8);
                uint32_t type = byteOrder_.read32(buf, 12);
                uint8_t format = buf[16];
                uint32_t numElements = byteOrder_.read32(buf, 20);
                const uint8_t* data = buf + 24;
                propertyStore_.change(window, property, type, format, mode, data, numElements);
                break;  // void, no reply
            }
            case DeleteProperty: {
                uint32_t window = byteOrder_.read32(buf, 4);
                uint32_t property = byteOrder_.read32(buf, 8);
                propertyStore_.remove(window, property);
                break;  // void, no reply
            }
            case GetProperty: {
                // Request: opcode(1), delete(1), length(2), window(4), property(4),
                //          type(4), offset(4), max_length(4)
                uint32_t window = byteOrder_.read32(buf, 4);
                uint32_t property = byteOrder_.read32(buf, 8);
                uint32_t reqType = byteOrder_.read32(buf, 12);
                uint32_t offset = byteOrder_.read32(buf, 16);
                uint32_t maxLen = byteOrder_.read32(buf, 20);
                bool deleteAfter = buf[1];

                uint32_t bytesAfter = 0;
                auto prop = propertyStore_.get(window, property, reqType, offset, maxLen, bytesAfter);

                size_t dataLen = prop.data.size();
                uint32_t pad = (4 - (dataLen % 4)) % 4;
                uint32_t replySize = 32 + dataLen + pad;
                std::vector<uint8_t> reply(replySize, 0);
                reply[0] = 1;
                reply[1] = prop.format;
                byteOrder_.write16(reply.data(), 2, seq);
                byteOrder_.write32(reply.data(), 4, (uint32_t)((dataLen + pad) / 4));
                byteOrder_.write32(reply.data(), 8, prop.type);
                byteOrder_.write32(reply.data(), 12, bytesAfter);
                uint32_t numElements = 0;
                if (prop.format > 0 && dataLen > 0)
                    numElements = (uint32_t)(dataLen / (prop.format / 8));
                byteOrder_.write32(reply.data(), 16, numElements);
                if (dataLen > 0)
                    memcpy(reply.data() + 32, prop.data.data(), dataLen);
                sendAll(serverFd_, reply.data(), replySize);

                if (deleteAfter && prop.format > 0)
                    propertyStore_.remove(window, property);
                break;
            }
            case 21: { // ListProperties
                uint32_t window = byteOrder_.read32(buf, 4);
                auto atoms = propertyStore_.list(window);
                uint32_t numAtoms = (uint32_t)atoms.size();
                uint32_t dataSize = numAtoms * 4;
                uint32_t replySize = 32 + dataSize;
                std::vector<uint8_t> reply(replySize, 0);
                reply[0] = 1;
                byteOrder_.write16(reply.data(), 2, seq);
                byteOrder_.write32(reply.data(), 4, numAtoms);
                byteOrder_.write16(reply.data(), 8, (uint16_t)numAtoms);
                for (uint32_t i = 0; i < numAtoms; i++)
                    byteOrder_.write32(reply.data(), 32 + i * 4, atoms[i]);
                sendAll(serverFd_, reply.data(), replySize);
                break;
            }
            // --- SendEvent ---
            case SendEvent: {
                // Request: opcode(1), propagate(1), length(2), destination(4), event_mask(4), event(32)
                // Forward the 32-byte event at buf+12 with bit 7 set
                byteOrder_.write16(buf + 12, 2, seq);  // rewrite seq
                buf[12] |= 0x80;  // mark as synthetic
                sendAll(serverFd_, buf + 12, 32);
                break;
            }
            // --- Image operations ---
            case PutImage: {
                // Header: opcode(1), format(1), length(2), drawable(4), gc(4),
                //         width(2), height(2), dst_x(2), dst_y(2), left_pad(1), depth(1), pad(2)
                uint32_t drawable = byteOrder_.read32(buf, 4);
                int w = (int)byteOrder_.read16(buf, 12);
                int h = (int)byteOrder_.read16(buf, 14);
                int x = (int)(int16_t)byteOrder_.read16(buf, 16);
                int y = (int)(int16_t)byteOrder_.read16(buf, 18);
                size_t pixelDataLen = (length >= 6) ? ((size_t)length * 4 - 24) : 0;
                const uint8_t* pixelData = buf + 24;

                if (isWindow(drawable)) {
                    std::vector<ClipRect> noClip;
                    framebuffer_.putImage(x, y, w, h, pixelData, pixelDataLen,
                                          byteOrder_.msbFirst, noClip);
                } else {
                    auto* pm = pixmapStore_.get(drawable);
                    if (pm && w > 0 && h > 0 && pixelDataLen >= (size_t)w * h * 4) {
                        // Simple blit into pixmap (LSB, no clipping)
                        const uint32_t* src = reinterpret_cast<const uint32_t*>(pixelData);
                        for (int row = 0; row < h && row + y < pm->h; row++) {
                            for (int col = 0; col < w && col + x < pm->w; col++) {
                                if (x + col >= 0 && y + row >= 0) {
                                    pm->pixels[(y + row) * pm->w + (x + col)] =
                                        src[row * w + col] | 0xFF000000u;
                                }
                            }
                        }
                    }
                }
                break;  // void, no reply
            }
            case GetImage: {
                uint32_t drawable = byteOrder_.read32(buf, 4);
                int gx = (int)(int16_t)byteOrder_.read16(buf, 8);
                int gy = (int)(int16_t)byteOrder_.read16(buf, 10);
                int gw = (int)byteOrder_.read16(buf, 12);
                int gh = (int)byteOrder_.read16(buf, 14);

                size_t imgBytes = (size_t)gw * gh * 4;
                size_t imgWords = (imgBytes + 3) / 4;
                size_t replySize = 32 + imgWords * 4;
                std::vector<uint8_t> reply(replySize, 0);
                reply[0] = 1;
                reply[1] = 24;  // depth
                byteOrder_.write16(reply.data(), 2, seq);
                byteOrder_.write32(reply.data(), 4, (uint32_t)imgWords);

                if (isWindow(drawable)) {
                    framebuffer_.getImage(gx, gy, gw, gh,
                                          reinterpret_cast<uint32_t*>(reply.data() + 32));
                } else {
                    auto* pm = pixmapStore_.get(drawable);
                    if (pm && gw > 0 && gh > 0) {
                        uint32_t* dst = reinterpret_cast<uint32_t*>(reply.data() + 32);
                        for (int row = 0; row < gh; row++) {
                            int sy = gy + row;
                            for (int col = 0; col < gw; col++) {
                                int sx = gx + col;
                                if (sx >= 0 && sx < pm->w && sy >= 0 && sy < pm->h) {
                                    dst[row * gw + col] = pm->pixels[sy * pm->w + sx];
                                }
                            }
                        }
                    }
                }
                sendAll(serverFd_, reply.data(), replySize);
                break;
            }
            case CreatePixmap: {
                uint32_t pid = byteOrder_.read32(buf, 4);
                int pw = (int)byteOrder_.read16(buf, 12);
                int ph = (int)byteOrder_.read16(buf, 14);
                pixmapStore_.create(pid, pw, ph);
                break;
            }
            case FreePixmap: {
                uint32_t pid = byteOrder_.read32(buf, 4);
                pixmapStore_.destroy(pid);
                break;
            }
            case CopyArea: {
                uint32_t srcId = byteOrder_.read32(buf, 4);
                uint32_t dstId = byteOrder_.read32(buf, 8);
                // gc at offset 12, skip
                int srcX = (int)(int16_t)byteOrder_.read16(buf, 16);
                int srcY = (int)(int16_t)byteOrder_.read16(buf, 18);
                int dstX = (int)(int16_t)byteOrder_.read16(buf, 20);
                int dstY = (int)(int16_t)byteOrder_.read16(buf, 22);
                int cw = (int)byteOrder_.read16(buf, 24);
                int ch = (int)byteOrder_.read16(buf, 26);

                auto src = resolveDrawable(srcId);
                auto dst = resolveDrawable(dstId);
                if (src.pixels && dst.pixels) {
                    X11Framebuffer::copyArea(src.pixels, src.w, src.h, srcX, srcY,
                                              dst.pixels, dst.w, dst.h, dstX, dstY,
                                              cw, ch);
                }
                break;
            }
            // --- GLX sub-protocol ---
            case kGLXMajorOpcode: {
                uint8_t glxMinor = buf[1];
                handleGLX(glxMinor, buf, length, seq);
                break;
            }
            default:
                break;  // Void/unknown requests: consume silently
        }
    }

    void handleGLX(uint8_t minor, uint8_t* buf, uint16_t length, uint16_t seq) {
        switch (minor) {
            case 7: { // glXQueryVersion
                uint8_t reply[32] = {};
                reply[0] = 1;
                byteOrder_.write16(reply, 2, seq);
                byteOrder_.write32(reply, 8, 1);   // major
                byteOrder_.write32(reply, 12, 4);  // minor
                sendAll(serverFd_, reply, 32);
                break;
            }
            case 5: // glXMakeCurrent
            case 24: { // glXMakeContextCurrent
                uint8_t reply[32] = {};
                reply[0] = 1;
                byteOrder_.write16(reply, 2, seq);
                byteOrder_.write32(reply, 8, 1);  // context_tag
                sendAll(serverFd_, reply, 32);
                break;
            }
            case 6: { // glXIsDirect
                uint8_t reply[32] = {};
                reply[0] = 1;
                byteOrder_.write16(reply, 2, seq);
                reply[8] = 0;  // is_direct = false (indirect)
                sendAll(serverFd_, reply, 32);
                break;
            }
            case 14: { // glXGetVisualConfigs
                uint32_t numConfigs = 1, numProps = 28;
                size_t dataSize = numConfigs * numProps * 4;
                size_t replySize = 32 + dataSize;
                std::vector<uint8_t> reply(replySize, 0);
                reply[0] = 1;
                byteOrder_.write16(reply.data(), 2, seq);
                byteOrder_.write32(reply.data(), 4, (uint32_t)(dataSize / 4));
                byteOrder_.write32(reply.data(), 8, numConfigs);
                byteOrder_.write32(reply.data(), 12, numProps);
                // Config properties at offset 32
                uint32_t props[28] = {
                    kDefaultVisualId, 4/*TrueColor*/, 1/*RGBA*/,
                    8, 8, 8, 8, // r,g,b,a bits
                    0, 0, 0, 0, // accum r,g,b,a
                    1, // double-buffer
                    0, // stereo
                    32, // buffer-size
                    24, // depth-size
                    8,  // stencil-size
                    0, 0, 0, 0, // aux, red-mask, green-mask, blue-mask
                    0, 0, 0, 0, 0, 0, 0, 0 // padding
                };
                for (uint32_t i = 0; i < 28; i++) {
                    byteOrder_.write32(reply.data(), 32 + i * 4, props[i]);
                }
                sendAll(serverFd_, reply.data(), replySize);
                break;
            }
            case 21: { // glXGetFBConfigs
                uint32_t numConfigs = 1, numAttribs = 28;
                size_t dataSize = numConfigs * numAttribs * 2 * 4; // key-value pairs
                size_t replySize = 32 + dataSize;
                std::vector<uint8_t> reply(replySize, 0);
                reply[0] = 1;
                byteOrder_.write16(reply.data(), 2, seq);
                byteOrder_.write32(reply.data(), 4, (uint32_t)(dataSize / 4));
                byteOrder_.write32(reply.data(), 8, numConfigs);
                byteOrder_.write32(reply.data(), 12, numAttribs);
                // Key-value pairs
                uint32_t kvs[][2] = {
                    {0x8013, 1}, // GLX_FBCONFIG_ID
                    {0x8010, 32}, // GLX_BUFFER_SIZE
                    {2, 4}, // GLX_LEVEL (placeholder)
                    {8, 8}, {9, 8}, {10, 8}, {11, 8}, // RED/GREEN/BLUE/ALPHA_SIZE
                    {12, 24}, // GLX_DEPTH_SIZE
                    {13, 8},  // GLX_STENCIL_SIZE
                    {5, 1},   // GLX_DOUBLEBUFFER
                    {0x8012, kDefaultVisualId}, // GLX_VISUAL_ID
                    {0x8015, 0x8014}, // GLX_DRAWABLE_TYPE = GLX_WINDOW_BIT
                    {0x8011, 0x8020}, // GLX_RENDER_TYPE = GLX_RGBA_BIT
                    {0x20, 4}, // GLX_X_VISUAL_TYPE = GLX_TRUE_COLOR
                    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
                    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
                    {0, 0}, {0, 0},
                };
                for (uint32_t i = 0; i < numAttribs && i < 28; i++) {
                    byteOrder_.write32(reply.data(), 32 + i * 8, kvs[i][0]);
                    byteOrder_.write32(reply.data(), 32 + i * 8 + 4, kvs[i][1]);
                }
                sendAll(serverFd_, reply.data(), replySize);
                break;
            }
            case 19: { // glXQueryServerString
                // Empty string reply
                uint8_t reply[36] = {};
                reply[0] = 1;
                byteOrder_.write16(reply, 2, seq);
                byteOrder_.write32(reply, 4, 1);  // length in 4-byte units (4 bytes of string data)
                byteOrder_.write32(reply, 8, 0);  // string length = 0
                sendAll(serverFd_, reply, 36);
                break;
            }
            case 26: { // glXQueryContext
                uint8_t reply[32] = {};
                reply[0] = 1;
                byteOrder_.write16(reply, 2, seq);
                sendAll(serverFd_, reply, 32);
                break;
            }
            // Void GLX ops (no reply): Render, RenderLarge, CreateContext, DestroyContext, SwapBuffers, etc.
            case 1: case 2: case 3: case 4: case 8: case 9: case 10: case 11:
            case 12: case 13: case 18: case 20: case 22:
                break;
            default: {
                // Unknown GLX sub-opcode: send generic reply to avoid desync
                uint8_t reply[32] = {};
                reply[0] = 1;
                byteOrder_.write16(reply, 2, seq);
                sendAll(serverFd_, reply, 32);
                break;
            }
        }
    }

    int serverFd_ = -1;
    int clientFd_ = -1;
    int listenFd_ = -1;
    int tcpDisplayNum_ = -1;
    int displayWidth_ = 800;
    int displayHeight_ = 600;
    std::atomic<bool> running_{false};
    std::thread serverThread_;

    X11ByteOrder byteOrder_{false};  // default LSB
    X11AtomStore atoms_;
    X11WindowManager windowManager_;
    X11PixmapStore pixmapStore_;
    X11PropertyStore propertyStore_;
    X11Framebuffer framebuffer_;
    X11EventBuilder eventBuilder_{byteOrder_};
};

// Helper to build and send a raw X11 request
inline bool sendRequest(int fd, const X11ByteOrder& bo, uint8_t opcode, uint8_t data1,
                        const std::vector<uint8_t>& body) {
    size_t totalLen = 4 + body.size();
    size_t padded = (totalLen + 3) & ~3;
    std::vector<uint8_t> req(padded, 0);
    req[0] = opcode;
    req[1] = data1;
    bo.write16(req.data(), 2, (uint16_t)(padded / 4));
    if (!body.empty()) memcpy(req.data() + 4, body.data(), body.size());

    const uint8_t* p = req.data();
    size_t rem = padded;
    while (rem > 0) {
        ssize_t n = send(fd, p, rem, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p += n; rem -= n;
    }
    return true;
}

// Helper to send raw bytes (for BigRequests tests etc.)
inline bool sendRaw(int fd, const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}

// Helper to receive exactly `len` bytes
inline bool recvExact(int fd, void* buf, size_t len, int timeoutMs = 2000) {
    uint8_t* p = (uint8_t*)buf;
    while (len > 0) {
        struct pollfd pfd = {fd, POLLIN, 0};
        int ret = poll(&pfd, 1, timeoutMs);
        if (ret <= 0) return false;
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}

} // namespace test
} // namespace guitarrackcraft
