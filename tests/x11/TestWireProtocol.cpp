#include <gtest/gtest.h>
#include "TestHelpers.h"
#include <unistd.h>

using namespace guitarrackcraft;
using namespace guitarrackcraft::test;

class WireProtocolTest : public ::testing::Test {
protected:
    X11TestServer server;
    X11ByteOrder bo{false};  // LSB
    int fd = -1;

    void SetUp() override {
        fd = server.start(800, 600);
        ASSERT_GE(fd, 0);
        // Perform connection handshake
        uint8_t req[12] = {};
        req[0] = 0x6c;  // LSB
        bo.write16(req, 2, 11);
        bo.write16(req, 4, 0);
        ASSERT_EQ(send(fd, req, 12, MSG_NOSIGNAL), 12);

        // Read 120-byte connection reply
        uint8_t reply[120];
        ASSERT_TRUE(recvExact(fd, reply, 120));
        ASSERT_EQ(reply[0], kX11ConnectionAccepted);
    }

    void TearDown() override {
        if (fd >= 0) close(fd);
        server.stop();
    }
};

TEST_F(WireProtocolTest, ConnectionHandshakeRoundTrip) {
    // Already verified in SetUp — if we get here, handshake succeeded
    SUCCEED();
}

TEST_F(WireProtocolTest, InternAtomRequest) {
    // InternAtom: opcode=16, only_if_exists=0, body: name_len(2) + pad(2) + name
    std::string name = "WM_NAME";
    std::vector<uint8_t> body(4 + name.size());
    bo.write16(body.data(), 0, (uint16_t)name.size());
    memcpy(body.data() + 4, name.data(), name.size());
    ASSERT_TRUE(sendRequest(fd, bo, 16, 0, body));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);  // Reply
    uint32_t atomId = bo.read32(reply, 8);
    EXPECT_NE(atomId, 0u);
}

TEST_F(WireProtocolTest, GetAtomNameRoundTrip) {
    // First intern an atom
    std::string name = "MY_TEST_ATOM";
    std::vector<uint8_t> body(4 + name.size());
    bo.write16(body.data(), 0, (uint16_t)name.size());
    memcpy(body.data() + 4, name.data(), name.size());
    ASSERT_TRUE(sendRequest(fd, bo, 16, 0, body));

    uint8_t internReply[32];
    ASSERT_TRUE(recvExact(fd, internReply, 32));
    uint32_t atomId = bo.read32(internReply, 8);

    // Now GetAtomName: opcode=17, body: atom_id(4)
    std::vector<uint8_t> body2(4);
    bo.write32(body2.data(), 0, atomId);
    ASSERT_TRUE(sendRequest(fd, bo, 17, 0, body2));

    // Read 32-byte header
    uint8_t nameReply[32];
    ASSERT_TRUE(recvExact(fd, nameReply, 32));
    EXPECT_EQ(nameReply[0], 1);
    uint32_t replyLen = bo.read32(nameReply, 4);  // additional data length in 4-byte units
    uint16_t nameLen = bo.read16(nameReply, 8);
    EXPECT_EQ(nameLen, name.size());

    // Read additional data
    if (replyLen > 0) {
        std::vector<uint8_t> extra(replyLen * 4);
        ASSERT_TRUE(recvExact(fd, extra.data(), extra.size()));
        std::string got(extra.begin(), extra.begin() + nameLen);
        EXPECT_EQ(got, name);
    }
}

TEST_F(WireProtocolTest, CreateWindowThenGetGeometry) {
    // CreateWindow: opcode=1, depth=24
    // body: wid(4), parent(4), x(2), y(2), w(2), h(2), border_width(2), class(2), visual(4), value_mask(4)
    std::vector<uint8_t> body(28, 0);
    bo.write32(body.data(), 0, 0x200001);     // wid
    bo.write32(body.data(), 4, kRootWindowId); // parent
    bo.write16(body.data(), 8, 10);            // x
    bo.write16(body.data(), 10, 20);           // y
    bo.write16(body.data(), 12, 400);          // width
    bo.write16(body.data(), 14, 300);          // height
    bo.write16(body.data(), 16, 0);            // border_width
    bo.write16(body.data(), 18, 1);            // class = InputOutput
    bo.write32(body.data(), 20, kDefaultVisualId); // visual
    bo.write32(body.data(), 24, 0);            // value_mask
    ASSERT_TRUE(sendRequest(fd, bo, 1, 24, body));

    // Consume the Expose event that the server sends on CreateWindow
    uint8_t exposeEvt[32];
    ASSERT_TRUE(recvExact(fd, exposeEvt, 32));
    EXPECT_EQ(exposeEvt[0], X11Event::Expose);

    // GetGeometry: opcode=14, body: drawable(4)
    std::vector<uint8_t> geomBody(4);
    bo.write32(geomBody.data(), 0, 0x200001);
    ASSERT_TRUE(sendRequest(fd, bo, 14, 0, geomBody));

    uint8_t geomReply[32];
    ASSERT_TRUE(recvExact(fd, geomReply, 32));
    EXPECT_EQ(geomReply[0], 1);  // Reply
    EXPECT_EQ((int16_t)bo.read16(geomReply, 12), 10);   // x
    EXPECT_EQ((int16_t)bo.read16(geomReply, 14), 20);   // y
    EXPECT_EQ(bo.read16(geomReply, 16), 400u);           // width
    EXPECT_EQ(bo.read16(geomReply, 18), 300u);           // height
}

TEST_F(WireProtocolTest, MapWindowSendsExpose) {
    // Create window first
    std::vector<uint8_t> createBody(28, 0);
    bo.write32(createBody.data(), 0, 0x200001);
    bo.write32(createBody.data(), 4, kRootWindowId);
    bo.write16(createBody.data(), 12, 200);  // w
    bo.write16(createBody.data(), 14, 150);  // h
    bo.write32(createBody.data(), 20, kDefaultVisualId);
    ASSERT_TRUE(sendRequest(fd, bo, 1, 24, createBody));

    uint8_t createExpose[32];
    ASSERT_TRUE(recvExact(fd, createExpose, 32));

    // MapWindow: opcode=8
    std::vector<uint8_t> mapBody(4);
    bo.write32(mapBody.data(), 0, 0x200001);
    ASSERT_TRUE(sendRequest(fd, bo, 8, 0, mapBody));

    uint8_t mapExpose[32];
    ASSERT_TRUE(recvExact(fd, mapExpose, 32));
    EXPECT_EQ(mapExpose[0], X11Event::Expose);
    EXPECT_EQ(bo.read32(mapExpose, 4), 0x200001u);
}

TEST_F(WireProtocolTest, QueryExtensionGLX) {
    std::string name = "GLX";
    std::vector<uint8_t> body(4 + name.size());
    bo.write16(body.data(), 0, (uint16_t)name.size());
    memcpy(body.data() + 4, name.data(), name.size());
    ASSERT_TRUE(sendRequest(fd, bo, 98, 0, body));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);
    EXPECT_EQ(reply[8], 1);    // present = true
    EXPECT_EQ(reply[9], 128);  // major_opcode = 128 (kGLXMajorOpcode)
}

TEST_F(WireProtocolTest, QueryExtensionUnknown) {
    std::string name = "XUnknown";
    std::vector<uint8_t> body(4 + name.size());
    bo.write16(body.data(), 0, (uint16_t)name.size());
    memcpy(body.data() + 4, name.data(), name.size());
    ASSERT_TRUE(sendRequest(fd, bo, 98, 0, body));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);
    EXPECT_EQ(reply[8], 0);  // present = false
}

TEST_F(WireProtocolTest, GetWindowAttributes) {
    // Create and map a window
    std::vector<uint8_t> createBody(28, 0);
    bo.write32(createBody.data(), 0, 0x200001);
    bo.write32(createBody.data(), 4, kRootWindowId);
    bo.write16(createBody.data(), 12, 100);
    bo.write16(createBody.data(), 14, 100);
    bo.write32(createBody.data(), 20, kDefaultVisualId);
    ASSERT_TRUE(sendRequest(fd, bo, 1, 24, createBody));
    uint8_t createExpose[32];
    ASSERT_TRUE(recvExact(fd, createExpose, 32));

    // Map
    std::vector<uint8_t> mapBody(4);
    bo.write32(mapBody.data(), 0, 0x200001);
    ASSERT_TRUE(sendRequest(fd, bo, 8, 0, mapBody));
    uint8_t mapExpose[32];
    ASSERT_TRUE(recvExact(fd, mapExpose, 32));

    // GetWindowAttributes: opcode=3
    std::vector<uint8_t> body(4);
    bo.write32(body.data(), 0, 0x200001);
    ASSERT_TRUE(sendRequest(fd, bo, 3, 0, body));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);
    // map-state at byte 26 per X11 spec: 2 = IsViewable (mapped)
    // Reply is 44 bytes: read additional 12 bytes beyond 32-byte header
    uint8_t extra[12];
    ASSERT_TRUE(recvExact(fd, extra, 12));
    // Byte 26 in the full reply = byte 26 - 0 (it's within the first 32 bytes)
    EXPECT_EQ(reply[26], 2);
}

TEST_F(WireProtocolTest, QueryPointerReply) {
    // QueryPointer: opcode=38, body: window(4)
    std::vector<uint8_t> body(4);
    bo.write32(body.data(), 0, kRootWindowId);
    ASSERT_TRUE(sendRequest(fd, bo, 38, 0, body));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);   // Reply
    EXPECT_EQ(reply[1], 1);   // same-screen = true
    EXPECT_EQ(bo.read32(reply, 8), kRootWindowId);  // root
}

TEST_F(WireProtocolTest, SequenceNumberIncrement) {
    // Send two InternAtom requests and verify sequence numbers increment
    auto sendIntern = [&](const std::string& name) -> uint16_t {
        std::vector<uint8_t> body(4 + name.size());
        bo.write16(body.data(), 0, (uint16_t)name.size());
        memcpy(body.data() + 4, name.data(), name.size());
        sendRequest(fd, bo, 16, 0, body);
        uint8_t reply[32];
        recvExact(fd, reply, 32);
        return bo.read16(reply, 2);
    };

    uint16_t seq1 = sendIntern("ATOM_A");
    uint16_t seq2 = sendIntern("ATOM_B");
    EXPECT_EQ(seq2, seq1 + 1);
}

TEST_F(WireProtocolTest, ListExtensions) {
    ASSERT_TRUE(sendRequest(fd, bo, 99, 0, {}));  // ListExtensions = 99 per X11 spec

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);
    uint8_t numExt = reply[1];
    EXPECT_GE(numExt, 1);

    uint32_t dataLen = bo.read32(reply, 4);
    if (dataLen > 0) {
        std::vector<uint8_t> data(dataLen * 4);
        ASSERT_TRUE(recvExact(fd, data.data(), data.size()));
        EXPECT_EQ(data[0], 3);
        EXPECT_EQ(std::string(data.begin() + 1, data.begin() + 4), "GLX");
    }
}

// === Phase 2a: Edge Cases & Robustness ===

TEST_F(WireProtocolTest, BigRequestsSkip) {
    // Send a request with length=0 (BigRequests format)
    // Header: opcode=254, pad=0, length=0
    uint8_t header[4] = {254, 0, 0, 0};
    ASSERT_TRUE(sendRaw(fd, header, 4));
    // Extended length: 3 (= 3*4 = 12 bytes total, 4 bytes of body after the 8-byte header)
    uint8_t extLen[4];
    bo.write32(extLen, 0, 3);
    ASSERT_TRUE(sendRaw(fd, extLen, 4));
    // Body: 4 bytes (bigLength=3 minus 2 for header+extlen = 1 word = 4 bytes)
    uint8_t body[4] = {0, 0, 0, 0};
    ASSERT_TRUE(sendRaw(fd, body, 4));

    // Now send a normal InternAtom — should still work
    std::string name = "AFTER_BIG";
    std::vector<uint8_t> atomBody(4 + name.size());
    bo.write16(atomBody.data(), 0, (uint16_t)name.size());
    memcpy(atomBody.data() + 4, name.data(), name.size());
    ASSERT_TRUE(sendRequest(fd, bo, 16, 0, atomBody));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);
    EXPECT_NE(bo.read32(reply, 8), 0u);
}

TEST_F(WireProtocolTest, UnknownOpcodeRecovery) {
    // Send request with unhandled opcode 254, length=1 (4 bytes, header only)
    std::vector<uint8_t> emptyBody;
    ASSERT_TRUE(sendRequest(fd, bo, 254, 0, emptyBody));

    // Send InternAtom — should work despite unknown opcode preceding it
    std::string name = "AFTER_UNKNOWN";
    std::vector<uint8_t> body(4 + name.size());
    bo.write16(body.data(), 0, (uint16_t)name.size());
    memcpy(body.data() + 4, name.data(), name.size());
    ASSERT_TRUE(sendRequest(fd, bo, 16, 0, body));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);
    EXPECT_NE(bo.read32(reply, 8), 0u);
}

TEST_F(WireProtocolTest, GetPropertyEmptyReply) {
    // GetProperty: opcode=20, body: delete(1 in data1), window(4), property(4), type(4), offset(4), length(4)
    std::vector<uint8_t> body(20, 0);
    bo.write32(body.data(), 0, kRootWindowId);  // window
    ASSERT_TRUE(sendRequest(fd, bo, 20, 0, body));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);       // Reply
    EXPECT_EQ(reply[1], 0);       // format = 0
    EXPECT_EQ(bo.read32(reply, 4), 0u);   // length = 0
    EXPECT_EQ(bo.read32(reply, 8), 0u);   // type = None
    EXPECT_EQ(bo.read32(reply, 16), 0u);  // value_length = 0
}

TEST_F(WireProtocolTest, ChangePropertyThenGetProperty) {
    // ChangeProperty (void): opcode=18
    std::vector<uint8_t> cpBody(20, 0);
    bo.write32(cpBody.data(), 0, kRootWindowId);
    ASSERT_TRUE(sendRequest(fd, bo, 18, 0, cpBody));

    // GetProperty — stream should be intact
    std::vector<uint8_t> gpBody(20, 0);
    bo.write32(gpBody.data(), 0, kRootWindowId);
    ASSERT_TRUE(sendRequest(fd, bo, 20, 0, gpBody));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);
}

TEST_F(WireProtocolTest, VoidBurstThenReply) {
    // Create a window first
    std::vector<uint8_t> createBody(28, 0);
    bo.write32(createBody.data(), 0, 0x200001);
    bo.write32(createBody.data(), 4, kRootWindowId);
    bo.write16(createBody.data(), 12, 100);
    bo.write16(createBody.data(), 14, 100);
    bo.write32(createBody.data(), 20, kDefaultVisualId);
    ASSERT_TRUE(sendRequest(fd, bo, 1, 24, createBody));
    uint8_t exposeEvt[32];
    ASSERT_TRUE(recvExact(fd, exposeEvt, 32));  // consume Expose

    // Send 100 alternating MapWindow/UnmapWindow (all void except MapWindow sends Expose)
    std::vector<uint8_t> winBody(4);
    bo.write32(winBody.data(), 0, 0x200001);
    for (int i = 0; i < 50; i++) {
        ASSERT_TRUE(sendRequest(fd, bo, 8, 0, winBody));   // MapWindow
        uint8_t evt[32];
        ASSERT_TRUE(recvExact(fd, evt, 32));  // consume Expose
        ASSERT_TRUE(sendRequest(fd, bo, 10, 0, winBody));  // UnmapWindow
    }

    // GetGeometry should still work correctly
    std::vector<uint8_t> geomBody(4);
    bo.write32(geomBody.data(), 0, 0x200001);
    ASSERT_TRUE(sendRequest(fd, bo, 14, 0, geomBody));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 1);
    EXPECT_EQ(bo.read16(reply, 16), 100u);  // width
    EXPECT_EQ(bo.read16(reply, 18), 100u);  // height
}

TEST_F(WireProtocolTest, ErrorReplyOnBadDrawable) {
    // GetGeometry on non-existent drawable
    std::vector<uint8_t> body(4);
    bo.write32(body.data(), 0, 0xFFFFFFFF);
    ASSERT_TRUE(sendRequest(fd, bo, 14, 0, body));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 0);  // Error indicator
    EXPECT_EQ(reply[1], 9);  // BadDrawable error code
    EXPECT_EQ(bo.read32(reply, 4), 0xFFFFFFFFu);  // bad value
}

// === Phase 3: Property round-trips ===

TEST_F(WireProtocolTest, PropertyChangeAndGet_String) {
    // ChangeProperty: mode=Replace(0), window=root, property=1, type=31(STRING), format=8
    std::string value = "TestWindow";
    std::vector<uint8_t> cpBody(20 + value.size(), 0);
    bo.write32(cpBody.data(), 0, kRootWindowId);  // window
    bo.write32(cpBody.data(), 4, 1);               // property atom
    bo.write32(cpBody.data(), 8, 31);              // type = STRING
    cpBody[12] = 8;                                 // format
    bo.write32(cpBody.data(), 16, (uint32_t)value.size());  // num elements
    memcpy(cpBody.data() + 20, value.data(), value.size());
    ASSERT_TRUE(sendRequest(fd, bo, 18, 0, cpBody));  // ChangeProperty, mode=0

    // GetProperty: window=root, property=1, type=0(any), offset=0, maxLen=100
    std::vector<uint8_t> gpBody(20, 0);
    bo.write32(gpBody.data(), 0, kRootWindowId);
    bo.write32(gpBody.data(), 4, 1);    // property
    bo.write32(gpBody.data(), 8, 0);    // type=any
    bo.write32(gpBody.data(), 12, 0);   // offset
    bo.write32(gpBody.data(), 16, 100); // maxLen
    ASSERT_TRUE(sendRequest(fd, bo, 20, 0, gpBody));

    uint8_t header[32];
    ASSERT_TRUE(recvExact(fd, header, 32));
    EXPECT_EQ(header[0], 1);       // Reply
    EXPECT_EQ(header[1], 8);       // format
    EXPECT_EQ(bo.read32(header, 8), 31u);  // type = STRING
    uint32_t numElements = bo.read32(header, 16);
    EXPECT_EQ(numElements, value.size());

    uint32_t dataLen = bo.read32(header, 4);
    if (dataLen > 0) {
        std::vector<uint8_t> data(dataLen * 4);
        ASSERT_TRUE(recvExact(fd, data.data(), data.size()));
        std::string got(data.begin(), data.begin() + numElements);
        EXPECT_EQ(got, value);
    }
}

TEST_F(WireProtocolTest, PropertyChangeAndGet_Cardinal32) {
    uint32_t values[3] = {42, 100, 0xDEADBEEF};
    std::vector<uint8_t> cpBody(20 + 12, 0);
    bo.write32(cpBody.data(), 0, kRootWindowId);
    bo.write32(cpBody.data(), 4, 2);    // property atom
    bo.write32(cpBody.data(), 8, 6);    // type = CARDINAL
    cpBody[12] = 32;                     // format
    bo.write32(cpBody.data(), 16, 3);   // num elements
    memcpy(cpBody.data() + 20, values, 12);
    ASSERT_TRUE(sendRequest(fd, bo, 18, 0, cpBody));

    std::vector<uint8_t> gpBody(20, 0);
    bo.write32(gpBody.data(), 0, kRootWindowId);
    bo.write32(gpBody.data(), 4, 2);
    bo.write32(gpBody.data(), 16, 100);
    ASSERT_TRUE(sendRequest(fd, bo, 20, 0, gpBody));

    uint8_t header[32];
    ASSERT_TRUE(recvExact(fd, header, 32));
    EXPECT_EQ(header[1], 32);  // format
    uint32_t numElements = bo.read32(header, 16);
    EXPECT_EQ(numElements, 3u);

    uint32_t dataLen = bo.read32(header, 4);
    if (dataLen > 0) {
        std::vector<uint8_t> data(dataLen * 4);
        ASSERT_TRUE(recvExact(fd, data.data(), data.size()));
        uint32_t got[3];
        memcpy(got, data.data(), 12);
        EXPECT_EQ(got[0], 42u);
        EXPECT_EQ(got[1], 100u);
        EXPECT_EQ(got[2], 0xDEADBEEFu);
    }
}

TEST_F(WireProtocolTest, PropertyDeleteThenGet) {
    // Set a property
    uint8_t value[] = "hello";
    std::vector<uint8_t> cpBody(20 + 5, 0);
    bo.write32(cpBody.data(), 0, kRootWindowId);
    bo.write32(cpBody.data(), 4, 3);
    bo.write32(cpBody.data(), 8, 31);
    cpBody[12] = 8;
    bo.write32(cpBody.data(), 16, 5);
    memcpy(cpBody.data() + 20, value, 5);
    ASSERT_TRUE(sendRequest(fd, bo, 18, 0, cpBody));

    // Delete it
    std::vector<uint8_t> dpBody(8, 0);
    bo.write32(dpBody.data(), 0, kRootWindowId);
    bo.write32(dpBody.data(), 4, 3);
    ASSERT_TRUE(sendRequest(fd, bo, 19, 0, dpBody));

    // Get it — should be empty
    std::vector<uint8_t> gpBody(20, 0);
    bo.write32(gpBody.data(), 0, kRootWindowId);
    bo.write32(gpBody.data(), 4, 3);
    bo.write32(gpBody.data(), 16, 100);
    ASSERT_TRUE(sendRequest(fd, bo, 20, 0, gpBody));

    uint8_t header[32];
    ASSERT_TRUE(recvExact(fd, header, 32));
    EXPECT_EQ(header[1], 0);  // format=0 means not found
    EXPECT_EQ(bo.read32(header, 16), 0u);  // value_length=0
}

// === Phase 3: Missing opcode tests ===

TEST_F(WireProtocolTest, DestroyWindowCleansUp) {
    // Create window
    std::vector<uint8_t> createBody(28, 0);
    bo.write32(createBody.data(), 0, 0x200001);
    bo.write32(createBody.data(), 4, kRootWindowId);
    bo.write16(createBody.data(), 12, 100);
    bo.write16(createBody.data(), 14, 100);
    bo.write32(createBody.data(), 20, kDefaultVisualId);
    ASSERT_TRUE(sendRequest(fd, bo, 1, 24, createBody));
    uint8_t evt[32];
    ASSERT_TRUE(recvExact(fd, evt, 32));  // Expose

    // Destroy it
    std::vector<uint8_t> destroyBody(4);
    bo.write32(destroyBody.data(), 0, 0x200001);
    ASSERT_TRUE(sendRequest(fd, bo, 4, 0, destroyBody));

    // GetGeometry should now return error
    std::vector<uint8_t> geomBody(4);
    bo.write32(geomBody.data(), 0, 0x200001);
    ASSERT_TRUE(sendRequest(fd, bo, 14, 0, geomBody));

    uint8_t reply[32];
    ASSERT_TRUE(recvExact(fd, reply, 32));
    EXPECT_EQ(reply[0], 0);  // Error
    EXPECT_EQ(reply[1], 9);  // BadDrawable
}

TEST_F(WireProtocolTest, UnmapWindowThenGetAttributes) {
    // Create and map
    std::vector<uint8_t> createBody(28, 0);
    bo.write32(createBody.data(), 0, 0x200001);
    bo.write32(createBody.data(), 4, kRootWindowId);
    bo.write16(createBody.data(), 12, 100);
    bo.write16(createBody.data(), 14, 100);
    bo.write32(createBody.data(), 20, kDefaultVisualId);
    ASSERT_TRUE(sendRequest(fd, bo, 1, 24, createBody));
    uint8_t evt1[32];
    ASSERT_TRUE(recvExact(fd, evt1, 32));

    std::vector<uint8_t> mapBody(4);
    bo.write32(mapBody.data(), 0, 0x200001);
    ASSERT_TRUE(sendRequest(fd, bo, 8, 0, mapBody));
    uint8_t evt2[32];
    ASSERT_TRUE(recvExact(fd, evt2, 32));

    // Unmap
    ASSERT_TRUE(sendRequest(fd, bo, 10, 0, mapBody));

    // GetWindowAttributes — map_state should be 0 (Unmapped)
    std::vector<uint8_t> attrBody(4);
    bo.write32(attrBody.data(), 0, 0x200001);
    ASSERT_TRUE(sendRequest(fd, bo, 3, 0, attrBody));

    uint8_t reply[44];
    ASSERT_TRUE(recvExact(fd, reply, 44));
    EXPECT_EQ(reply[0], 1);
    EXPECT_EQ(reply[26], 0);  // map_state = Unmapped
}

TEST_F(WireProtocolTest, SendEventForwarding) {
    // SendEvent: opcode=25, propagate=0, destination=root, event_mask=0, event=32 bytes
    std::vector<uint8_t> body(40, 0);
    bo.write32(body.data(), 0, kRootWindowId);  // destination
    bo.write32(body.data(), 4, 0);               // event_mask

    // Build a synthetic Expose event at offset 8
    body[8] = 12;  // Expose event type
    bo.write32(body.data() + 12, 0, kRootWindowId);  // window
    bo.write16(body.data() + 16, 0, 100);  // width
    bo.write16(body.data() + 18, 0, 50);   // height
    ASSERT_TRUE(sendRequest(fd, bo, 25, 0, body));

    // Should receive the event with bit 7 set (synthetic)
    uint8_t evt[32];
    ASSERT_TRUE(recvExact(fd, evt, 32));
    EXPECT_EQ(evt[0] & 0x7F, 12);   // Expose
    EXPECT_NE(evt[0] & 0x80, 0);    // Synthetic flag set
}
