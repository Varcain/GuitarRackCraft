#include <gtest/gtest.h>
#include "TestHelpers.h"
#include <unistd.h>

using namespace guitarrackcraft;
using namespace guitarrackcraft::test;

class GLXWireTest : public ::testing::Test {
protected:
    X11TestServer server;
    X11ByteOrder bo{false};
    int fd = -1;

    void SetUp() override {
        fd = server.start(800, 600);
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

    // Send GLX sub-request: opcode=128, data1=glxMinor
    bool sendGLX(uint8_t glxMinor, const std::vector<uint8_t>& body = {}) {
        return sendRequest(fd, bo, 128, glxMinor, body);
    }
};

TEST_F(GLXWireTest, GLXQueryVersion) {
    // glXQueryVersion(7): body has client major(4) + minor(4)
    std::vector<uint8_t> body(8, 0);
    bo.write32(body.data(), 0, 1);  // client major
    bo.write32(body.data(), 4, 4);  // client minor
    ASSERT_TRUE(sendGLX(7, body));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);
    EXPECT_EQ(bo.read32(reply, 8), 1u);   // server major
    EXPECT_EQ(bo.read32(reply, 12), 4u);  // server minor
}

TEST_F(GLXWireTest, GLXMakeCurrentSuccess) {
    std::vector<uint8_t> body(12, 0);  // drawable, context, old_context_tag
    ASSERT_TRUE(sendGLX(5, body));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);
    EXPECT_NE(bo.read32(reply, 8), 0u);  // context_tag != 0
}

TEST_F(GLXWireTest, GLXMakeContextCurrentSuccess) {
    std::vector<uint8_t> body(16, 0);
    ASSERT_TRUE(sendGLX(24, body));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);
    EXPECT_NE(bo.read32(reply, 8), 0u);
}

TEST_F(GLXWireTest, GLXIsDirectReturnsFalse) {
    std::vector<uint8_t> body(4, 0);  // context
    ASSERT_TRUE(sendGLX(6, body));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);
    EXPECT_EQ(reply[8], 0);  // is_direct = false
}

TEST_F(GLXWireTest, GLXGetVisualConfigs) {
    std::vector<uint8_t> body(4, 0);  // screen number
    ASSERT_TRUE(sendGLX(14, body));

    uint8_t header[32];
    ASSERT_TRUE(recvExact(fd, header, 32));
    EXPECT_EQ(header[0], 1);
    uint32_t numConfigs = bo.read32(header, 8);
    uint32_t numProps = bo.read32(header, 12);
    EXPECT_EQ(numConfigs, 1u);
    EXPECT_EQ(numProps, 28u);

    uint32_t dataLen = bo.read32(header, 4);
    ASSERT_GT(dataLen, 0u);
    std::vector<uint8_t> data(dataLen * 4);
    ASSERT_TRUE(recvExact(fd, data.data(), data.size()));

    // First config props
    EXPECT_EQ(bo.read32(data.data(), 0), kDefaultVisualId);  // visual ID
    EXPECT_EQ(bo.read32(data.data(), 4), 4u);   // TrueColor
    EXPECT_EQ(bo.read32(data.data(), 8), 1u);   // RGBA
    EXPECT_EQ(bo.read32(data.data(), 12), 8u);  // red bits
    EXPECT_EQ(bo.read32(data.data(), 16), 8u);  // green bits
    EXPECT_EQ(bo.read32(data.data(), 20), 8u);  // blue bits
    EXPECT_EQ(bo.read32(data.data(), 24), 8u);  // alpha bits
}

TEST_F(GLXWireTest, GLXGetFBConfigs) {
    std::vector<uint8_t> body(4, 0);
    ASSERT_TRUE(sendGLX(21, body));

    uint8_t header[32];
    ASSERT_TRUE(recvExact(fd, header, 32));
    EXPECT_EQ(header[0], 1);
    uint32_t numConfigs = bo.read32(header, 8);
    uint32_t numAttribs = bo.read32(header, 12);
    EXPECT_EQ(numConfigs, 1u);
    EXPECT_EQ(numAttribs, 28u);

    uint32_t dataLen = bo.read32(header, 4);
    ASSERT_GT(dataLen, 0u);
    std::vector<uint8_t> data(dataLen * 4);
    ASSERT_TRUE(recvExact(fd, data.data(), data.size()));

    // First key-value pair: GLX_FBCONFIG_ID (0x8013) = 1
    EXPECT_EQ(bo.read32(data.data(), 0), 0x8013u);
    EXPECT_EQ(bo.read32(data.data(), 4), 1u);
    // Second: GLX_BUFFER_SIZE (0x8010) = 32
    EXPECT_EQ(bo.read32(data.data(), 8), 0x8010u);
    EXPECT_EQ(bo.read32(data.data(), 12), 32u);
}

TEST_F(GLXWireTest, GLXQueryServerString) {
    std::vector<uint8_t> body(8, 0);  // screen, name
    ASSERT_TRUE(sendGLX(19, body));

    uint8_t header[32];
    ASSERT_TRUE(recvExact(fd, header, 32));
    EXPECT_EQ(header[0], 1);
    uint32_t replyLen = bo.read32(header, 4);
    EXPECT_EQ(replyLen, 1u);  // 4 bytes of string data
    uint32_t strLen = bo.read32(header, 8);
    EXPECT_EQ(strLen, 0u);    // empty string

    if (replyLen > 0) {
        std::vector<uint8_t> data(replyLen * 4);
        recvExact(fd, data.data(), data.size());
    }
}

TEST_F(GLXWireTest, GLXQueryContext) {
    std::vector<uint8_t> body(4, 0);
    ASSERT_TRUE(sendGLX(26, body));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);
}

TEST_F(GLXWireTest, GLXVoidOpNoReply) {
    // glXSwapBuffers (11) is a void op — no reply expected
    std::vector<uint8_t> body(8, 0);
    ASSERT_TRUE(sendGLX(11, body));

    // Immediately send InternAtom to verify no desync
    std::string name = "GLX_TEST";
    std::vector<uint8_t> atomBody(4 + name.size());
    bo.write16(atomBody.data(), 0, (uint16_t)name.size());
    memcpy(atomBody.data() + 4, name.data(), name.size());
    ASSERT_TRUE(sendRequest(fd, bo, 16, 0, atomBody));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);  // InternAtom reply, not a stale GLX reply
    EXPECT_NE(bo.read32(reply, 8), 0u);
}

TEST_F(GLXWireTest, GLXUnknownSubOpcode) {
    // Sub-opcode 99 should get a generic 32-byte reply
    std::vector<uint8_t> body(4, 0);
    ASSERT_TRUE(sendGLX(99, body));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);

    // Verify next request still works
    std::string name = "AFTER_GLX_UNKNOWN";
    std::vector<uint8_t> atomBody(4 + name.size());
    bo.write16(atomBody.data(), 0, (uint16_t)name.size());
    memcpy(atomBody.data() + 4, name.data(), name.size());
    ASSERT_TRUE(sendRequest(fd, bo, 16, 0, atomBody));

    uint8_t atomReply[32];
    ASSERT_TRUE(recvExact(fd, atomReply, 32));
    EXPECT_EQ(atomReply[0], 1);
    EXPECT_NE(bo.read32(atomReply, 8), 0u);
}
