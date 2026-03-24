#include <gtest/gtest.h>
#include "TestHelpers.h"
#include <unistd.h>

using namespace guitarrackcraft;
using namespace guitarrackcraft::test;

class ImageOpsWireTest : public ::testing::Test {
protected:
    X11TestServer server;
    X11ByteOrder bo{false};  // LSB
    int fd = -1;

    void SetUp() override {
        fd = server.start(64, 64);  // Small framebuffer for image tests
        ASSERT_GE(fd, 0);
        uint8_t req[12] = {};
        req[0] = 0x6c;
        bo.write16(req, 2, 11);
        ASSERT_EQ(send(fd, req, 12, MSG_NOSIGNAL), 12);
        uint8_t reply[120];
        ASSERT_TRUE(recvExact(fd, reply, 120));
    }

    void TearDown() override {
        if (fd >= 0) close(fd);
        server.stop();
    }

    // Helper: build PutImage request body (after 4-byte header)
    // Format: drawable(4), gc(4), width(2), height(2), dst_x(2), dst_y(2), left_pad(1), depth(1), pad(2) + pixels
    std::vector<uint8_t> buildPutImageBody(uint32_t drawable, int w, int h, int x, int y,
                                            const uint32_t* pixels) {
        size_t pixelBytes = (size_t)w * h * 4;
        std::vector<uint8_t> body(20 + pixelBytes, 0);
        bo.write32(body.data(), 0, drawable);   // drawable
        bo.write32(body.data(), 4, 0);          // gc (ignored)
        bo.write16(body.data(), 8, (uint16_t)w);
        bo.write16(body.data(), 10, (uint16_t)h);
        bo.write16(body.data(), 12, (uint16_t)(int16_t)x);
        bo.write16(body.data(), 14, (uint16_t)(int16_t)y);
        body[16] = 0;   // left_pad
        body[17] = 24;  // depth
        memcpy(body.data() + 20, pixels, pixelBytes);
        return body;
    }

    // Helper: send PutImage
    bool putImage(uint32_t drawable, int w, int h, int x, int y, const uint32_t* pixels) {
        auto body = buildPutImageBody(drawable, w, h, x, y, pixels);
        return sendRequest(fd, bo, 72, 2/*ZPixmap*/, body);
    }

    // Helper: send GetImage, receive pixel data
    bool getImage(uint32_t drawable, int x, int y, int w, int h, std::vector<uint32_t>& outPixels) {
        std::vector<uint8_t> body(16, 0);
        bo.write32(body.data(), 0, drawable);
        bo.write16(body.data(), 4, (uint16_t)(int16_t)x);
        bo.write16(body.data(), 6, (uint16_t)(int16_t)y);
        bo.write16(body.data(), 8, (uint16_t)w);
        bo.write16(body.data(), 10, (uint16_t)h);
        bo.write32(body.data(), 12, 0xFFFFFFFF);  // plane_mask
        if (!sendRequest(fd, bo, 73, 2/*ZPixmap*/, body)) return false;

        uint8_t header[32];
        if (!recvExact(fd, header, 32)) return false;
        if (header[0] != 1) return false;

        uint32_t imgWords = bo.read32(header, 4);
        if (imgWords > 0) {
            std::vector<uint8_t> data(imgWords * 4);
            if (!recvExact(fd, data.data(), data.size())) return false;
            outPixels.resize(w * h);
            memcpy(outPixels.data(), data.data(), std::min(data.size(), (size_t)w * h * 4));
        } else {
            outPixels.assign(w * h, 0);
        }
        return true;
    }

    // Helper: create pixmap
    bool createPixmap(uint32_t pid, int w, int h) {
        std::vector<uint8_t> body(12, 0);
        bo.write32(body.data(), 0, pid);           // pid
        bo.write32(body.data(), 4, kRootWindowId); // drawable (for depth)
        bo.write16(body.data(), 8, (uint16_t)w);
        bo.write16(body.data(), 10, (uint16_t)h);
        return sendRequest(fd, bo, 53, 24/*depth*/, body);
    }

    // Helper: send CopyArea
    bool copyArea(uint32_t src, uint32_t dst, int srcX, int srcY, int dstX, int dstY, int w, int h) {
        std::vector<uint8_t> body(24, 0);
        bo.write32(body.data(), 0, src);
        bo.write32(body.data(), 4, dst);
        bo.write32(body.data(), 8, 0);  // gc
        bo.write16(body.data(), 12, (uint16_t)(int16_t)srcX);
        bo.write16(body.data(), 14, (uint16_t)(int16_t)srcY);
        bo.write16(body.data(), 16, (uint16_t)(int16_t)dstX);
        bo.write16(body.data(), 18, (uint16_t)(int16_t)dstY);
        bo.write16(body.data(), 20, (uint16_t)w);
        bo.write16(body.data(), 22, (uint16_t)h);
        return sendRequest(fd, bo, 62, 0, body);
    }
};

TEST_F(ImageOpsWireTest, PutImageGetImageRoundTrip_LSB) {
    uint32_t pixels[16];  // 4x4
    for (int i = 0; i < 16; i++) pixels[i] = 0x00110000 + i;  // unique per pixel

    ASSERT_TRUE(putImage(kRootWindowId, 4, 4, 0, 0, pixels));

    std::vector<uint32_t> got;
    ASSERT_TRUE(getImage(kRootWindowId, 0, 0, 4, 4, got));
    ASSERT_EQ(got.size(), 16u);

    for (int i = 0; i < 16; i++) {
        EXPECT_EQ(got[i], pixels[i] | 0xFF000000u)
            << "Pixel " << i << " mismatch";
    }
}

TEST_F(ImageOpsWireTest, PutImageGetImageRoundTrip_Offset) {
    uint32_t pixels[4] = {0x00AABB01, 0x00AABB02, 0x00AABB03, 0x00AABB04};
    ASSERT_TRUE(putImage(kRootWindowId, 2, 2, 10, 15, pixels));

    std::vector<uint32_t> got;
    ASSERT_TRUE(getImage(kRootWindowId, 10, 15, 2, 2, got));
    ASSERT_EQ(got.size(), 4u);
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(got[i], pixels[i] | 0xFF000000u);
    }
}

TEST_F(ImageOpsWireTest, PutImageGetImage_PartialOutOfBounds) {
    // Write 4x4 at (0,0)
    uint32_t pixels[16];
    for (int i = 0; i < 16; i++) pixels[i] = 0x00FF0000 + i;
    ASSERT_TRUE(putImage(kRootWindowId, 4, 4, 0, 0, pixels));

    // GetImage at (62,62) with 4x4 — only 2x2 is in-bounds (framebuffer is 64x64)
    std::vector<uint32_t> got;
    ASSERT_TRUE(getImage(kRootWindowId, 62, 62, 4, 4, got));
    ASSERT_EQ(got.size(), 16u);

    // In-bounds pixels (0,0) and (1,0) and (0,1) and (1,1) should be the default fill
    // Out-of-bounds pixels should be 0
    EXPECT_NE(got[0], 0u);   // (62,62) is in-bounds
    EXPECT_EQ(got[2], 0u);   // (64,62) is out-of-bounds
    EXPECT_EQ(got[3], 0u);   // (65,62) is out-of-bounds
}

TEST_F(ImageOpsWireTest, PutImageToPixmap_GetImageFromPixmap) {
    uint32_t pmId = 0x300001;
    ASSERT_TRUE(createPixmap(pmId, 8, 8));

    uint32_t pixels[4] = {0x00112233, 0x00445566, 0x00778899, 0x00AABBCC};
    ASSERT_TRUE(putImage(pmId, 2, 2, 0, 0, pixels));

    std::vector<uint32_t> got;
    ASSERT_TRUE(getImage(pmId, 0, 0, 2, 2, got));
    ASSERT_EQ(got.size(), 4u);
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(got[i], pixels[i] | 0xFF000000u);
    }
}

TEST_F(ImageOpsWireTest, CopyAreaFramebufferToPixmap) {
    uint32_t pixels[4] = {0x00AA0001, 0x00AA0002, 0x00AA0003, 0x00AA0004};
    ASSERT_TRUE(putImage(kRootWindowId, 2, 2, 0, 0, pixels));

    uint32_t pmId = 0x300001;
    ASSERT_TRUE(createPixmap(pmId, 8, 8));
    ASSERT_TRUE(copyArea(kRootWindowId, pmId, 0, 0, 0, 0, 2, 2));

    std::vector<uint32_t> got;
    ASSERT_TRUE(getImage(pmId, 0, 0, 2, 2, got));
    ASSERT_EQ(got.size(), 4u);
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(got[i], pixels[i] | 0xFF000000u);
    }
}

TEST_F(ImageOpsWireTest, CopyAreaPixmapToFramebuffer) {
    uint32_t pmId = 0x300001;
    ASSERT_TRUE(createPixmap(pmId, 8, 8));

    uint32_t pixels[4] = {0x00BB0001, 0x00BB0002, 0x00BB0003, 0x00BB0004};
    ASSERT_TRUE(putImage(pmId, 2, 2, 0, 0, pixels));
    ASSERT_TRUE(copyArea(pmId, kRootWindowId, 0, 0, 5, 5, 2, 2));

    std::vector<uint32_t> got;
    ASSERT_TRUE(getImage(kRootWindowId, 5, 5, 2, 2, got));
    ASSERT_EQ(got.size(), 4u);
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(got[i], pixels[i] | 0xFF000000u);
    }
}

TEST_F(ImageOpsWireTest, CopyAreaPixmapToPixmap) {
    uint32_t pm1 = 0x300001, pm2 = 0x300002;
    ASSERT_TRUE(createPixmap(pm1, 8, 8));
    ASSERT_TRUE(createPixmap(pm2, 8, 8));

    uint32_t pixels[4] = {0x00CC0001, 0x00CC0002, 0x00CC0003, 0x00CC0004};
    ASSERT_TRUE(putImage(pm1, 2, 2, 0, 0, pixels));
    ASSERT_TRUE(copyArea(pm1, pm2, 0, 0, 3, 3, 2, 2));

    std::vector<uint32_t> got;
    ASSERT_TRUE(getImage(pm2, 3, 3, 2, 2, got));
    ASSERT_EQ(got.size(), 4u);
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(got[i], pixels[i] | 0xFF000000u);
    }
}

TEST_F(ImageOpsWireTest, GetImageUninitializedFramebuffer) {
    // GetImage before any PutImage — should return default fill color (0xFF302020)
    std::vector<uint32_t> got;
    ASSERT_TRUE(getImage(kRootWindowId, 0, 0, 2, 2, got));
    ASSERT_EQ(got.size(), 4u);
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(got[i], 0xFF302020u);
    }
}

TEST_F(ImageOpsWireTest, PutImageGetImage_MSB) {
    // Start a separate server with MSB byte order
    X11TestServer msbServer;
    X11ByteOrder msbBo{true};
    int msbFd = msbServer.start(32, 32);
    ASSERT_GE(msbFd, 0);

    uint8_t req[12] = {};
    req[0] = 0x42;  // MSB
    msbBo.write16(req, 2, 11);
    ASSERT_EQ(send(msbFd, req, 12, MSG_NOSIGNAL), 12);
    uint8_t connReply[120];
    ASSERT_TRUE(recvExact(msbFd, connReply, 120));

    // PutImage a 2x2 pattern
    uint32_t pixels[4] = {0x00110022, 0x00330044, 0x00550066, 0x00770088};
    size_t pixelBytes = 16;
    std::vector<uint8_t> body(20 + pixelBytes, 0);
    msbBo.write32(body.data(), 0, kRootWindowId);
    msbBo.write16(body.data(), 8, 2);   // width
    msbBo.write16(body.data(), 10, 2);  // height
    msbBo.write16(body.data(), 12, 0);  // x
    msbBo.write16(body.data(), 14, 0);  // y
    body[17] = 24;
    memcpy(body.data() + 20, pixels, pixelBytes);
    ASSERT_TRUE(sendRequest(msbFd, msbBo, 72, 2, body));

    // GetImage
    std::vector<uint8_t> giBody(16, 0);
    msbBo.write32(giBody.data(), 0, kRootWindowId);
    msbBo.write16(giBody.data(), 8, 2);
    msbBo.write16(giBody.data(), 10, 2);
    msbBo.write32(giBody.data(), 12, 0xFFFFFFFF);
    ASSERT_TRUE(sendRequest(msbFd, msbBo, 73, 2, giBody));

    uint8_t header[32];
    ASSERT_TRUE(recvExact(msbFd, header, 32));
    EXPECT_EQ(header[0], 1);
    uint32_t imgWords = msbBo.read32(header, 4);
    ASSERT_GT(imgWords, 0u);

    std::vector<uint8_t> data(imgWords * 4);
    ASSERT_TRUE(recvExact(msbFd, data.data(), data.size()));

    // Verify pixels were stored (alpha forced)
    // The exact format depends on MSB encoding path in framebuffer
    // At minimum, verify we got non-zero data back
    bool anyNonDefault = false;
    for (size_t i = 0; i < data.size(); i += 4) {
        uint32_t pixel;
        memcpy(&pixel, data.data() + i, 4);
        if (pixel != 0xFF302020u && pixel != 0) anyNonDefault = true;
    }
    EXPECT_TRUE(anyNonDefault) << "MSB PutImage/GetImage should return written pixels";

    close(msbFd);
    msbServer.stop();
}

TEST_F(ImageOpsWireTest, CopyAreaOverlappingSrcDst) {
    // Write 4x4 at (0,0)
    uint32_t pixels[16];
    for (int i = 0; i < 16; i++) pixels[i] = 0x00DD0000 + i;
    ASSERT_TRUE(putImage(kRootWindowId, 4, 4, 0, 0, pixels));

    // CopyArea from (0,0) to (2,0) — overlapping by 2 columns
    ASSERT_TRUE(copyArea(kRootWindowId, kRootWindowId, 0, 0, 2, 0, 4, 4));

    // Read the destination region
    std::vector<uint32_t> got;
    ASSERT_TRUE(getImage(kRootWindowId, 2, 0, 4, 4, got));
    // First two columns should have the original (0,0)-(1,3) data
    EXPECT_EQ(got[0], (pixels[0] | 0xFF000000u));
    EXPECT_EQ(got[1], (pixels[1] | 0xFF000000u));
}

TEST_F(ImageOpsWireTest, PutImageZeroSize) {
    // PutImage with w=0 — should be a no-op, not crash
    uint32_t dummy = 0;
    ASSERT_TRUE(putImage(kRootWindowId, 0, 0, 0, 0, &dummy));

    // Verify framebuffer is still intact by reading default fill
    std::vector<uint32_t> got;
    ASSERT_TRUE(getImage(kRootWindowId, 0, 0, 1, 1, got));
    EXPECT_EQ(got[0], 0xFF302020u);  // default fill unchanged
}

TEST_F(ImageOpsWireTest, CreatePixmapZeroSize) {
    // CreatePixmap with w=0 — should not crash
    ASSERT_TRUE(createPixmap(0x300099, 0, 0));
    // GetImage on it — should return something (possibly empty)
    // Just verify no crash/hang
    std::vector<uint32_t> got;
    // Don't try to read 0x0 — just verify we can send the next request
    std::string name = "AFTER_ZERO_PIXMAP";
    std::vector<uint8_t> body(4 + name.size());
    bo.write16(body.data(), 0, (uint16_t)name.size());
    memcpy(body.data() + 4, name.data(), name.size());
    ASSERT_TRUE(sendRequest(fd, bo, 16, 0, body));
    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);  // InternAtom reply OK
}
