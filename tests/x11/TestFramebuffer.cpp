#include <gtest/gtest.h>
#include "X11Framebuffer.h"

using namespace guitarrackcraft;

TEST(Framebuffer, ResizeSetsSize) {
    X11Framebuffer fb;
    fb.resize(800, 600);
    EXPECT_EQ(fb.width(), 800);
    EXPECT_EQ(fb.height(), 600);
    EXPECT_EQ(fb.pixelCount(), 800u * 600u);
}

TEST(Framebuffer, ResizeFillsColor) {
    X11Framebuffer fb;
    fb.resize(4, 4, 0xFFAABBCC);
    for (size_t i = 0; i < fb.pixelCount(); i++) {
        EXPECT_EQ(fb.data()[i], 0xFFAABBCC);
    }
}

TEST(Framebuffer, PutImageBasicLSB) {
    X11Framebuffer fb;
    fb.resize(4, 4, 0);

    // 2x2 image, LSB-first: each pixel is a uint32_t memcpy'd from wire bytes.
    // On little-endian host: bytes {B, G, R, A} → uint32_t 0xAARRGGBB
    // Pixel: R=0xFF, G=0, B=0, A=0 → wire bytes {0x00, 0x00, 0xFF, 0x00} → uint32 0x00FF0000
    uint8_t pixels[16] = {
        0x00, 0x00, 0xFF, 0x00,  // pixel (0,0): red (B=0,G=0,R=FF,A=0)
        0x00, 0xFF, 0x00, 0x00,  // pixel (1,0): green
        0xFF, 0x00, 0x00, 0x00,  // pixel (0,1): blue
        0xFF, 0xFF, 0xFF, 0x00,  // pixel (1,1): white
    };
    std::vector<ClipRect> noClip;
    fb.putImage(0, 0, 2, 2, pixels, sizeof(pixels), false, noClip);

    // Alpha forced: 0x00FF0000 | 0xFF000000 = 0xFFFF0000
    EXPECT_EQ(fb.data()[0], 0xFFFF0000u);  // red with alpha forced
    EXPECT_EQ(fb.data()[1], 0xFF00FF00u);  // green with alpha forced
    EXPECT_EQ(fb.data()[4 + 0], 0xFF0000FFu);  // blue with alpha forced
    EXPECT_EQ(fb.data()[4 + 1], 0xFFFFFFFFu);  // white with alpha forced
}

TEST(Framebuffer, PutImageAlphaForcing) {
    X11Framebuffer fb;
    fb.resize(2, 1, 0);

    // All zero pixel — alpha should still be forced to 0xFF
    uint8_t pixels[4] = {0, 0, 0, 0};
    std::vector<ClipRect> noClip;
    fb.putImage(0, 0, 1, 1, pixels, 4, false, noClip);
    // The stored pixel should have alpha byte set (0xFF000000)
    EXPECT_EQ(fb.data()[0] & 0xFF000000u, 0xFF000000u);
}

TEST(Framebuffer, PutImageOffset) {
    X11Framebuffer fb;
    fb.resize(4, 4, 0);

    uint8_t pixel[4] = {0xAA, 0xBB, 0xCC, 0x00};  // single pixel
    std::vector<ClipRect> noClip;
    fb.putImage(2, 3, 1, 1, pixel, 4, false, noClip);

    // Only pixel at (2,3) should be set
    EXPECT_NE(fb.data()[3 * 4 + 2], 0u);
    EXPECT_EQ(fb.data()[0], 0u);  // (0,0) untouched
}

TEST(Framebuffer, PutImageClipping) {
    X11Framebuffer fb;
    fb.resize(4, 4, 0);

    // 4x4 image placed at (2,2) — only a 2x2 portion fits
    uint8_t pixels[64];
    memset(pixels, 0xAA, sizeof(pixels));
    std::vector<ClipRect> noClip;
    fb.putImage(2, 2, 4, 4, pixels, sizeof(pixels), false, noClip);

    // Pixels at (2,2), (3,2), (2,3), (3,3) should be set
    EXPECT_NE(fb.data()[2 * 4 + 2], 0u);
    EXPECT_NE(fb.data()[3 * 4 + 3], 0u);
    // Pixel at (0,0) should be untouched
    EXPECT_EQ(fb.data()[0], 0u);
}

TEST(Framebuffer, PutImageChildClipping) {
    X11Framebuffer fb;
    fb.resize(8, 8, 0);

    // Fill entire 8x8 with solid color
    uint8_t pixels[256];
    memset(pixels, 0xFF, sizeof(pixels));
    std::vector<ClipRect> childClip = {{2, 2, 6, 6}};  // Child window at (2,2) to (6,6)
    fb.putImage(0, 0, 8, 8, pixels, sizeof(pixels), false, childClip);

    // Pixel at (0,0) should be set (outside child)
    EXPECT_NE(fb.data()[0], 0u);
    // Pixel at (3,3) should NOT be set (inside child clip rect)
    EXPECT_EQ(fb.data()[3 * 8 + 3], 0u);
    // Pixel at (7,7) should be set (outside child)
    EXPECT_NE(fb.data()[7 * 8 + 7], 0u);
}

TEST(Framebuffer, PutImageMSB) {
    X11Framebuffer fb;
    fb.resize(2, 1, 0);

    // MSB-first pixel: [A, R, G, B]
    uint8_t pixel[4] = {0x00, 0x11, 0x22, 0x33};
    std::vector<ClipRect> noClip;
    fb.putImage(0, 0, 1, 1, pixel, 4, true, noClip);

    // Should be stored with alpha forced
    uint32_t stored = fb.data()[0];
    EXPECT_EQ(stored & 0xFF000000u, 0xFF000000u);  // alpha forced
}

TEST(Framebuffer, GetImageFullyCovered) {
    X11Framebuffer fb;
    fb.resize(4, 4, 0);

    // Set specific pixels
    fb.data()[0] = 0xFFAABBCC;
    fb.data()[1] = 0xFF112233;
    fb.data()[4] = 0xFF445566;
    fb.data()[5] = 0xFF778899;

    uint32_t dst[4];
    fb.getImage(0, 0, 2, 2, dst);
    EXPECT_EQ(dst[0], 0xFFAABBCC);
    EXPECT_EQ(dst[1], 0xFF112233);
    EXPECT_EQ(dst[2], 0xFF445566);
    EXPECT_EQ(dst[3], 0xFF778899);
}

TEST(Framebuffer, GetImagePartialOutOfBounds) {
    X11Framebuffer fb;
    fb.resize(4, 4, 0xFFAAAAAA);

    // Request extends beyond right/bottom edge
    uint32_t dst[9];
    memset(dst, 0xFF, sizeof(dst));
    fb.getImage(2, 2, 3, 3, dst);

    // In-bounds pixels should be filled
    EXPECT_EQ(dst[0], 0xFFAAAAAA);  // (2,2)
    EXPECT_EQ(dst[1], 0xFFAAAAAA);  // (3,2)
    // Out-of-bounds should be zero
    EXPECT_EQ(dst[2], 0u);  // (4,2) - out of bounds
    EXPECT_EQ(dst[8], 0u);  // (4,4) - out of bounds
}

TEST(Framebuffer, GetImageNegativeOffset) {
    X11Framebuffer fb;
    fb.resize(4, 4, 0xFFBBBBBB);

    uint32_t dst[4];
    memset(dst, 0, sizeof(dst));
    fb.getImage(-1, -1, 2, 2, dst);

    // Only (0,0) of framebuffer maps to dst[3] (dst row 1, col 1)
    EXPECT_EQ(dst[0], 0u);          // maps to (-1,-1) — out of bounds
    EXPECT_EQ(dst[3], 0xFFBBBBBB);  // maps to (0,0) — in bounds
}

TEST(Framebuffer, PutGetRoundTrip) {
    X11Framebuffer fb;
    fb.resize(8, 8, 0);

    // Write a known pattern
    uint32_t srcPixels[4] = {0x00112233, 0x00445566, 0x00778899, 0x00AABBCC};
    uint8_t srcBytes[16];
    memcpy(srcBytes, srcPixels, 16);

    std::vector<ClipRect> noClip;
    fb.putImage(2, 3, 2, 2, srcBytes, 16, false, noClip);

    // Read it back
    uint32_t dst[4];
    fb.getImage(2, 3, 2, 2, dst);

    // Each pixel should have alpha forced to 0xFF
    EXPECT_EQ(dst[0], 0xFF112233u);
    EXPECT_EQ(dst[1], 0xFF445566u);
    EXPECT_EQ(dst[2], 0xFF778899u);
    EXPECT_EQ(dst[3], 0xFFAABBCC);
}

TEST(Framebuffer, CopyAreaNoOverlap) {
    X11Framebuffer fb;
    fb.resize(8, 8, 0);

    // Set pixels at (0,0)-(2,2)
    fb.data()[0] = 0xFF111111;
    fb.data()[1] = 0xFF222222;
    fb.data()[8] = 0xFF333333;
    fb.data()[9] = 0xFF444444;

    X11Framebuffer::copyArea(fb.data(), 8, 8, 0, 0,
                              fb.data(), 8, 8, 4, 4,
                              2, 2);

    EXPECT_EQ(fb.data()[4 * 8 + 4], 0xFF111111u);
    EXPECT_EQ(fb.data()[4 * 8 + 5], 0xFF222222u);
    EXPECT_EQ(fb.data()[5 * 8 + 4], 0xFF333333u);
    EXPECT_EQ(fb.data()[5 * 8 + 5], 0xFF444444u);
    // Source should be unchanged
    EXPECT_EQ(fb.data()[0], 0xFF111111u);
}

TEST(Framebuffer, CopyAreaClipping) {
    X11Framebuffer fb;
    fb.resize(4, 4, 0);
    fb.data()[0] = 0xFFAABBCC;

    // Copy from (0,0) to (3,3) with 2x2 — only 1x1 fits
    X11Framebuffer::copyArea(fb.data(), 4, 4, 0, 0,
                              fb.data(), 4, 4, 3, 3,
                              2, 2);

    EXPECT_EQ(fb.data()[3 * 4 + 3], 0xFFAABBCC);
}

TEST(Framebuffer, CopyAreaBetweenBuffers) {
    X11Framebuffer src;
    src.resize(4, 4, 0xFFAAAAAA);

    X11Framebuffer dst;
    dst.resize(4, 4, 0);

    X11Framebuffer::copyArea(src.data(), 4, 4, 1, 1,
                              dst.data(), 4, 4, 0, 0,
                              2, 2);

    EXPECT_EQ(dst.data()[0], 0xFFAAAAAA);
    EXPECT_EQ(dst.data()[1], 0xFFAAAAAA);
    EXPECT_EQ(dst.data()[4], 0xFFAAAAAA);
    EXPECT_EQ(dst.data()[5], 0xFFAAAAAA);
    // Rest should be zero
    EXPECT_EQ(dst.data()[2], 0u);
}

TEST(Framebuffer, ClearResetsState) {
    X11Framebuffer fb;
    fb.resize(100, 100);
    fb.clear();
    EXPECT_EQ(fb.width(), 0);
    EXPECT_EQ(fb.height(), 0);
    EXPECT_TRUE(fb.empty());
}

TEST(Framebuffer, EmptyFramebufferPutImageNoOp) {
    X11Framebuffer fb;
    uint8_t pixels[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    std::vector<ClipRect> noClip;
    // Should not crash on empty framebuffer
    fb.putImage(0, 0, 1, 1, pixels, 4, false, noClip);
}

TEST(Framebuffer, EmptyFramebufferGetImage) {
    X11Framebuffer fb;
    uint32_t dst[4] = {0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF};
    fb.getImage(0, 0, 2, 2, dst);
    // Should zero the output
    for (auto& p : dst) EXPECT_EQ(p, 0u);
}
