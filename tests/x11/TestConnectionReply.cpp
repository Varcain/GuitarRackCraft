#include <gtest/gtest.h>
#include "X11ConnectionHandler.h"
#include "X11Protocol.h"

using namespace guitarrackcraft;

class ConnectionReplyTest : public ::testing::TestWithParam<bool> {
protected:
    X11ByteOrder bo{GetParam()};
};

TEST_P(ConnectionReplyTest, Size120Bytes) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    EXPECT_EQ(reply.size(), 120u);
}

TEST_P(ConnectionReplyTest, AcceptedByte) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    EXPECT_EQ(reply[0], kX11ConnectionAccepted);
}

TEST_P(ConnectionReplyTest, ProtocolVersion) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    EXPECT_EQ(bo.read16(reply.data(), 2), kX11Major);  // 11
    EXPECT_EQ(bo.read16(reply.data(), 4), kX11Minor);  // 0
}

TEST_P(ConnectionReplyTest, AdditionalDataLength) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    // Additional data length in 4-byte units: (120 - 8) / 4 = 28
    EXPECT_EQ(bo.read16(reply.data(), 6), 28);
}

TEST_P(ConnectionReplyTest, ResourceIdBaseAndMask) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    uint32_t base = bo.read32(reply.data(), 12);  // offset 8 + 4 = 12
    uint32_t mask = bo.read32(reply.data(), 16);   // offset 8 + 8 = 16
    EXPECT_EQ(base, 0x00200000u);
    EXPECT_EQ(mask, 0x001FFFFFu);
    // Base and mask must be disjoint
    EXPECT_EQ(base & mask, 0u);
}

TEST_P(ConnectionReplyTest, NumRootsAndFormats) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    // num_roots at offset 8 + 18 + 2 = 28, num_formats at 29
    EXPECT_EQ(reply[28], 1);  // 1 root screen
    EXPECT_EQ(reply[29], 1);  // 1 pixmap format
}

TEST_P(ConnectionReplyTest, ImageByteOrder) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    uint8_t expected = bo.msbFirst ? 1 : 0;
    EXPECT_EQ(reply[30], expected);  // image byte order
    EXPECT_EQ(reply[31], expected);  // bitmap bit order
}

TEST_P(ConnectionReplyTest, KeycodeRange) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    // min keycode at offset 34, max at 35
    EXPECT_GE(reply[34], 8);  // min keycode must be >= 8 per X11 spec
    EXPECT_EQ(reply[35], 255);
}

TEST_P(ConnectionReplyTest, PixmapFormat) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    // Pixmap format starts at offset 40
    EXPECT_EQ(reply[40], 24);  // depth
    EXPECT_EQ(reply[41], 32);  // bits_per_pixel
    EXPECT_EQ(bo.read16(reply.data(), 42), 32u);  // scanline_pad
}

TEST_P(ConnectionReplyTest, RootWindowId) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    // Root window starts at offset 48
    EXPECT_EQ(bo.read32(reply.data(), 48), kRootWindowId);
}

TEST_P(ConnectionReplyTest, RootScreenDimensions) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    // Width at offset 48 + 20 = 68, height at 70
    EXPECT_EQ(bo.read16(reply.data(), 68), 800);
    EXPECT_EQ(bo.read16(reply.data(), 70), 600);
}

TEST_P(ConnectionReplyTest, RootVisualId) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    // root_visual at offset 48 + 32 = 80
    EXPECT_EQ(bo.read32(reply.data(), 80), kDefaultVisualId);
}

TEST_P(ConnectionReplyTest, RootDepth) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    // root-depth at offset 48 + 38 = 86
    EXPECT_EQ(reply[86], 24);
}

TEST_P(ConnectionReplyTest, TrueColorVisual) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    // Depth starts at 88, VisualType at 96
    EXPECT_EQ(reply[88], 24);  // depth value
    // Visual at offset 96:
    EXPECT_EQ(bo.read32(reply.data(), 96), kDefaultVisualId);  // visual ID
    EXPECT_EQ(reply[100], 4);   // class = TrueColor
    EXPECT_EQ(reply[101], 8);   // bits_per_rgb (per channel)
    EXPECT_EQ(bo.read16(reply.data(), 102), 256u);  // colormap_entries
    EXPECT_EQ(bo.read32(reply.data(), 104), 0xFF0000u);  // red_mask
    EXPECT_EQ(bo.read32(reply.data(), 108), 0x00FF00u);  // green_mask
    EXPECT_EQ(bo.read32(reply.data(), 112), 0x0000FFu);  // blue_mask
}

TEST_P(ConnectionReplyTest, MSBvsLSBDifferentEncoding) {
    X11ByteOrder lsbBo{false};
    X11ByteOrder msbBo{true};
    auto lsbReply = X11ConnectionHandler::buildConnectionReply(lsbBo, 800, 600);
    auto msbReply = X11ConnectionHandler::buildConnectionReply(msbBo, 800, 600);

    // Both should be 120 bytes
    EXPECT_EQ(lsbReply.size(), msbReply.size());
    // The raw bytes should differ (different byte order encoding)
    EXPECT_NE(lsbReply, msbReply);
    // But the accepted byte should be the same
    EXPECT_EQ(lsbReply[0], msbReply[0]);
}

INSTANTIATE_TEST_SUITE_P(
    ByteOrders,
    ConnectionReplyTest,
    ::testing::Values(false, true),
    [](const ::testing::TestParamInfo<bool>& info) {
        return info.param ? "MSB" : "LSB";
    }
);
