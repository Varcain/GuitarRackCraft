#include <gtest/gtest.h>
#include "X11ConnectionHandler.h"
#include "X11Protocol.h"
#include <string>

using namespace guitarrackcraft;

// Layout of the 132-byte setup reply (the one the Java X server sends and
// wine was verified against): 8-byte header, 32-byte fixed info, vendor
// string at 40, pixmap format at 52, screen at 60, depth at 100, visual at
// 108.
class ConnectionReplyTest : public ::testing::TestWithParam<bool> {
protected:
    X11ByteOrder bo{GetParam()};
};

TEST_P(ConnectionReplyTest, Size132Bytes) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    EXPECT_EQ(reply.size(), 132u);
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
    // Additional data length in 4-byte units: (132 - 8) / 4 = 31
    EXPECT_EQ(bo.read16(reply.data(), 6), 31);
    EXPECT_EQ(8u + bo.read16(reply.data(), 6) * 4u, reply.size());
}

TEST_P(ConnectionReplyTest, ResourceIdBaseAndMask) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    uint32_t base = bo.read32(reply.data(), 12);
    uint32_t mask = bo.read32(reply.data(), 16);
    EXPECT_EQ(base, 0x00100000u);  // default base
    EXPECT_EQ(mask, 0x000FFFFFu);
    // Base and mask must be disjoint
    EXPECT_EQ(base & mask, 0u);
}

TEST_P(ConnectionReplyTest, ResourceIdBaseIsPerConnection) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600, 0x00300000);
    EXPECT_EQ(bo.read32(reply.data(), 12), 0x00300000u);
}

TEST_P(ConnectionReplyTest, VendorString) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    ASSERT_EQ(bo.read16(reply.data(), 24), 11);  // vendor length
    EXPECT_EQ(std::string(reply.begin() + 40, reply.begin() + 51), "Open source");
}

TEST_P(ConnectionReplyTest, MaxRequestLength) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    EXPECT_EQ(bo.read16(reply.data(), 26), 0x7FFF);
}

TEST_P(ConnectionReplyTest, NumRootsAndFormats) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    EXPECT_EQ(reply[28], 1);  // 1 root screen
    EXPECT_EQ(reply[29], 1);  // 1 pixmap format
}

TEST_P(ConnectionReplyTest, ImageByteOrder) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    // The server's own image format, independent of the client's byte order:
    // pixel data is always LSB first.
    EXPECT_EQ(reply[30], 0);  // image byte order = LSBFirst
    EXPECT_EQ(reply[31], 1);  // bitmap bit order = MSBFirst
}

TEST_P(ConnectionReplyTest, KeycodeRange) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    EXPECT_GE(reply[34], 8);  // min keycode must be >= 8 per X11 spec
    EXPECT_EQ(reply[35], 164);
}

TEST_P(ConnectionReplyTest, PixmapFormat) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    EXPECT_EQ(reply[52], 32);  // depth
    EXPECT_EQ(reply[53], 32);  // bits_per_pixel
    EXPECT_EQ(reply[54], 32);  // scanline_pad
}

TEST_P(ConnectionReplyTest, RootWindowId) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    EXPECT_EQ(bo.read32(reply.data(), 60), kRootWindowId);
}

TEST_P(ConnectionReplyTest, RootScreenDimensions) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    EXPECT_EQ(bo.read16(reply.data(), 80), 800);
    EXPECT_EQ(bo.read16(reply.data(), 82), 600);
    EXPECT_EQ(bo.read16(reply.data(), 84), 65);  // width in mm
    EXPECT_EQ(bo.read16(reply.data(), 86), 27);  // height in mm
}

TEST_P(ConnectionReplyTest, RootVisualId) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    EXPECT_EQ(bo.read32(reply.data(), 92), kDefaultVisualId);
}

TEST_P(ConnectionReplyTest, RootDepth) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    EXPECT_EQ(reply[98], 32);  // root depth, matches the pixmap format
    EXPECT_EQ(reply[99], 1);   // one allowed depth
}

TEST_P(ConnectionReplyTest, TrueColorVisual) {
    auto reply = X11ConnectionHandler::buildConnectionReply(bo, 800, 600);
    EXPECT_EQ(reply[100], 32);                      // depth value
    EXPECT_EQ(bo.read16(reply.data(), 102), 1u);    // visuals at this depth
    EXPECT_EQ(bo.read32(reply.data(), 108), kDefaultVisualId);  // visual ID
    EXPECT_EQ(reply[112], 4);   // class = TrueColor
    EXPECT_EQ(reply[113], 8);   // bits_per_rgb (per channel)
    EXPECT_EQ(bo.read16(reply.data(), 114), 256u);  // colormap_entries
    EXPECT_EQ(bo.read32(reply.data(), 116), 0xFF0000u);  // red_mask
    EXPECT_EQ(bo.read32(reply.data(), 120), 0x00FF00u);  // green_mask
    EXPECT_EQ(bo.read32(reply.data(), 124), 0x0000FFu);  // blue_mask
}

TEST_P(ConnectionReplyTest, MSBvsLSBDifferentEncoding) {
    X11ByteOrder lsbBo{false};
    X11ByteOrder msbBo{true};
    auto lsbReply = X11ConnectionHandler::buildConnectionReply(lsbBo, 800, 600);
    auto msbReply = X11ConnectionHandler::buildConnectionReply(msbBo, 800, 600);

    // Same size in both byte orders
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
