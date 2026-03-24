#include <gtest/gtest.h>
#include "X11ByteOrder.h"

using guitarrackcraft::X11ByteOrder;

TEST(ByteOrder, Read16MSB) {
    X11ByteOrder bo{true};
    uint8_t data[] = {0x12, 0x34};
    EXPECT_EQ(bo.read16(data, 0), 0x1234);
}

TEST(ByteOrder, Read16LSB) {
    X11ByteOrder bo{false};
    uint8_t data[] = {0x34, 0x12};
    EXPECT_EQ(bo.read16(data, 0), 0x1234);
}

TEST(ByteOrder, Read32MSB) {
    X11ByteOrder bo{true};
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(bo.read32(data, 0), 0xDEADBEEFu);
}

TEST(ByteOrder, Read32LSB) {
    X11ByteOrder bo{false};
    uint8_t data[] = {0xEF, 0xBE, 0xAD, 0xDE};
    EXPECT_EQ(bo.read32(data, 0), 0xDEADBEEFu);
}

TEST(ByteOrder, Write16MSB) {
    X11ByteOrder bo{true};
    uint8_t data[2] = {};
    bo.write16(data, 0, 0x1234);
    EXPECT_EQ(data[0], 0x12);
    EXPECT_EQ(data[1], 0x34);
}

TEST(ByteOrder, Write16LSB) {
    X11ByteOrder bo{false};
    uint8_t data[2] = {};
    bo.write16(data, 0, 0x1234);
    EXPECT_EQ(data[0], 0x34);
    EXPECT_EQ(data[1], 0x12);
}

TEST(ByteOrder, Write32MSB) {
    X11ByteOrder bo{true};
    uint8_t data[4] = {};
    bo.write32(data, 0, 0xDEADBEEF);
    EXPECT_EQ(data[0], 0xDE);
    EXPECT_EQ(data[1], 0xAD);
    EXPECT_EQ(data[2], 0xBE);
    EXPECT_EQ(data[3], 0xEF);
}

TEST(ByteOrder, Write32LSB) {
    X11ByteOrder bo{false};
    uint8_t data[4] = {};
    bo.write32(data, 0, 0xDEADBEEF);
    EXPECT_EQ(data[0], 0xEF);
    EXPECT_EQ(data[1], 0xBE);
    EXPECT_EQ(data[2], 0xAD);
    EXPECT_EQ(data[3], 0xDE);
}

TEST(ByteOrder, RoundTrip16) {
    for (int msb = 0; msb <= 1; msb++) {
        X11ByteOrder bo{msb == 1};
        uint8_t buf[2];
        for (uint32_t v = 0; v <= 0xFFFF; v++) {
            bo.write16(buf, 0, (uint16_t)v);
            EXPECT_EQ(bo.read16(buf, 0), (uint16_t)v)
                << "Failed for value " << v << " msb=" << msb;
        }
    }
}

TEST(ByteOrder, RoundTrip32) {
    uint32_t values[] = {0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678, 0xDEADBEEF};
    for (int msb = 0; msb <= 1; msb++) {
        X11ByteOrder bo{msb == 1};
        uint8_t buf[4];
        for (uint32_t v : values) {
            bo.write32(buf, 0, v);
            EXPECT_EQ(bo.read32(buf, 0), v)
                << "Failed for value 0x" << std::hex << v << " msb=" << msb;
        }
    }
}

TEST(ByteOrder, OffsetAccess) {
    X11ByteOrder bo{true};
    uint8_t data[8] = {0, 0, 0x12, 0x34, 0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(bo.read16(data, 2), 0x1234);
    EXPECT_EQ(bo.read32(data, 4), 0xDEADBEEFu);

    uint8_t out[8] = {};
    bo.write16(out, 3, 0xABCD);
    EXPECT_EQ(out[3], 0xAB);
    EXPECT_EQ(out[4], 0xCD);

    bo.write32(out, 2, 0x11223344);
    EXPECT_EQ(out[2], 0x11);
    EXPECT_EQ(out[3], 0x22);
    EXPECT_EQ(out[4], 0x33);
    EXPECT_EQ(out[5], 0x44);
}

TEST(ByteOrder, ZeroValues) {
    for (int msb = 0; msb <= 1; msb++) {
        X11ByteOrder bo{msb == 1};
        uint8_t buf[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        bo.write16(buf, 0, 0);
        EXPECT_EQ(buf[0], 0);
        EXPECT_EQ(buf[1], 0);
        EXPECT_EQ(bo.read16(buf, 0), 0);

        bo.write32(buf, 0, 0);
        EXPECT_EQ(buf[0], 0);
        EXPECT_EQ(buf[1], 0);
        EXPECT_EQ(buf[2], 0);
        EXPECT_EQ(buf[3], 0);
        EXPECT_EQ(bo.read32(buf, 0), 0u);
    }
}
