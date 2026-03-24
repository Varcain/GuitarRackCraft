#include <gtest/gtest.h>
#include "X11ConnectionHandler.h"

using namespace guitarrackcraft;

TEST(ConnectionHandshake, DetectMSBByteOrder) {
    // MSB-first connection request: byte order 0x42
    uint8_t req[12] = {};
    req[0] = 0x42;  // MSB first
    X11ByteOrder msb{true};
    msb.write16(req, 2, 11);  // major version
    msb.write16(req, 4, 0);   // minor version

    auto result = X11ConnectionHandler::parseConnectionRequest(req);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.msbFirst);
}

TEST(ConnectionHandshake, DetectLSBByteOrder) {
    uint8_t req[12] = {};
    req[0] = 0x6c;  // LSB first
    X11ByteOrder lsb{false};
    lsb.write16(req, 2, 11);
    lsb.write16(req, 4, 0);

    auto result = X11ConnectionHandler::parseConnectionRequest(req);
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.msbFirst);
}

TEST(ConnectionHandshake, InvalidByteOrderFails) {
    uint8_t req[12] = {};
    req[0] = 0xFF;  // Invalid

    auto result = X11ConnectionHandler::parseConnectionRequest(req);
    EXPECT_FALSE(result.success);
}

TEST(ConnectionHandshake, ProtocolVersion) {
    uint8_t req[12] = {};
    req[0] = 0x6c;
    X11ByteOrder lsb{false};
    lsb.write16(req, 2, 11);
    lsb.write16(req, 4, 0);

    auto result = X11ConnectionHandler::parseConnectionRequest(req);
    EXPECT_EQ(result.majorVersion, 11);
    EXPECT_EQ(result.minorVersion, 0);
}

TEST(ConnectionHandshake, AuthLengths) {
    uint8_t req[12] = {};
    req[0] = 0x6c;
    X11ByteOrder lsb{false};
    lsb.write16(req, 2, 11);
    lsb.write16(req, 4, 0);
    lsb.write16(req, 6, 18);   // auth name length
    lsb.write16(req, 8, 16);   // auth data length

    auto result = X11ConnectionHandler::parseConnectionRequest(req);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.authNameLen, 18);
    EXPECT_EQ(result.authDataLen, 16);
}

TEST(ConnectionHandshake, EmptyAuth) {
    uint8_t req[12] = {};
    req[0] = 0x42;
    X11ByteOrder msb{true};
    msb.write16(req, 2, 11);
    msb.write16(req, 4, 0);
    msb.write16(req, 6, 0);
    msb.write16(req, 8, 0);

    auto result = X11ConnectionHandler::parseConnectionRequest(req);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.authNameLen, 0);
    EXPECT_EQ(result.authDataLen, 0);
}
