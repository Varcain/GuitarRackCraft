#include <gtest/gtest.h>
#include "X11EventBuilder.h"

using namespace guitarrackcraft;

class EventBuilderTest : public ::testing::Test {
protected:
    X11ByteOrder lsb{false};
    X11ByteOrder msb{true};
    X11EventBuilder lsbBuilder{lsb};
    X11EventBuilder msbBuilder{msb};
};

TEST_F(EventBuilderTest, ExposeFormat) {
    auto evt = lsbBuilder.expose(0x200001, 0, 0, 400, 300, 0, 42);
    EXPECT_EQ(evt[0], X11Event::Expose);
    EXPECT_EQ(lsb.read16(evt.data(), 2), 42);     // sequence
    EXPECT_EQ(lsb.read32(evt.data(), 4), 0x200001u); // window
    EXPECT_EQ(lsb.read16(evt.data(), 8), 0);       // x
    EXPECT_EQ(lsb.read16(evt.data(), 10), 0);      // y
    EXPECT_EQ(lsb.read16(evt.data(), 12), 400);    // width
    EXPECT_EQ(lsb.read16(evt.data(), 14), 300);    // height
    EXPECT_EQ(lsb.read16(evt.data(), 16), 0);      // count
}

TEST_F(EventBuilderTest, ButtonPressFormat) {
    auto evt = lsbBuilder.buttonPress(0x200001, 100, 200, 1, 5, 12345);
    EXPECT_EQ(evt[0], X11Event::ButtonPress);
    EXPECT_EQ(evt[1], 1);  // button detail
    EXPECT_EQ(lsb.read16(evt.data(), 2), 5);       // sequence
    EXPECT_EQ(lsb.read32(evt.data(), 4), 12345u);  // timestamp
    EXPECT_EQ(lsb.read32(evt.data(), 8), kRootWindowId); // root
    EXPECT_EQ(lsb.read32(evt.data(), 12), 0x200001u); // event window
    EXPECT_EQ(lsb.read32(evt.data(), 16), 0u);     // child (None)
    EXPECT_EQ((int16_t)lsb.read16(evt.data(), 24), 100); // event-x
    EXPECT_EQ((int16_t)lsb.read16(evt.data(), 26), 200); // event-y
    EXPECT_EQ(lsb.read16(evt.data(), 28), 0);      // state (no buttons during press)
}

TEST_F(EventBuilderTest, ButtonReleaseStateBits) {
    auto evt = lsbBuilder.buttonRelease(0x200001, 50, 60, 1, 10, 99999);
    EXPECT_EQ(evt[0], X11Event::ButtonRelease);
    EXPECT_EQ(evt[1], 1);  // button detail
    // On release of button 1, state should contain Button1Mask (0x100)
    EXPECT_EQ(lsb.read16(evt.data(), 28), 0x0100);
}

TEST_F(EventBuilderTest, MotionNotifyStateBits) {
    uint16_t button1Mask = 1 << 8;
    auto evt = lsbBuilder.motionNotify(0x200001, 75, 80, button1Mask, 15, 50000);
    EXPECT_EQ(evt[0], X11Event::MotionNotify);
    EXPECT_EQ(evt[1], 0);  // detail = 0 for motion
    EXPECT_EQ(lsb.read16(evt.data(), 28), button1Mask);
}

TEST_F(EventBuilderTest, ConfigureNotifyFormat) {
    auto evt = lsbBuilder.configureNotify(0x200001, 10, 20, 640, 480, 0, 7);
    EXPECT_EQ(evt[0], X11Event::ConfigureNotify);
    EXPECT_EQ(lsb.read16(evt.data(), 2), 7);        // sequence
    EXPECT_EQ(lsb.read32(evt.data(), 4), 0x200001u); // event
    EXPECT_EQ(lsb.read32(evt.data(), 8), 0x200001u); // window
    EXPECT_EQ(lsb.read32(evt.data(), 12), 0u);       // above-sibling (None)
    EXPECT_EQ((int16_t)lsb.read16(evt.data(), 16), 10);  // x
    EXPECT_EQ((int16_t)lsb.read16(evt.data(), 18), 20);  // y
    EXPECT_EQ(lsb.read16(evt.data(), 20), 640);      // width
    EXPECT_EQ(lsb.read16(evt.data(), 22), 480);      // height
    EXPECT_EQ(lsb.read16(evt.data(), 24), 0);        // border-width
    EXPECT_EQ(evt[26], 0);                            // override-redirect = False
}

TEST_F(EventBuilderTest, DestroyNotifyFormat) {
    auto evt = lsbBuilder.destroyNotify(0x200001, 99);
    EXPECT_EQ(evt[0], X11Event::DestroyNotify);
    EXPECT_EQ(lsb.read16(evt.data(), 2), 99);        // sequence
    EXPECT_EQ(lsb.read32(evt.data(), 4), 0x200001u); // event
    EXPECT_EQ(lsb.read32(evt.data(), 8), 0x200001u); // window
}

TEST_F(EventBuilderTest, MSBvsLSBEncoding) {
    auto lsbEvt = lsbBuilder.expose(0x200001, 0, 0, 100, 50, 0, 42);
    auto msbEvt = msbBuilder.expose(0x200001, 0, 0, 100, 50, 0, 42);

    // Both should be Expose events
    EXPECT_EQ(lsbEvt[0], X11Event::Expose);
    EXPECT_EQ(msbEvt[0], X11Event::Expose);

    // But the byte encoding of multi-byte fields should differ
    // Sequence 42: LSB = {0x2A, 0x00}, MSB = {0x00, 0x2A}
    EXPECT_EQ(lsbEvt[2], 0x2A);
    EXPECT_EQ(lsbEvt[3], 0x00);
    EXPECT_EQ(msbEvt[2], 0x00);
    EXPECT_EQ(msbEvt[3], 0x2A);
}

TEST_F(EventBuilderTest, SameScreenBit) {
    auto evt = lsbBuilder.buttonPress(0x200001, 0, 0, 1, 0, 0);
    EXPECT_EQ(evt[30], 1);  // same-screen = True

    auto evt2 = lsbBuilder.motionNotify(0x200001, 0, 0, 0, 0, 0);
    EXPECT_EQ(evt2[30], 1);
}

TEST_F(EventBuilderTest, AllEventsAre32Bytes) {
    EXPECT_EQ(lsbBuilder.buttonPress(1, 0, 0, 1, 0, 0).size(), 32u);
    EXPECT_EQ(lsbBuilder.buttonRelease(1, 0, 0, 1, 0, 0).size(), 32u);
    EXPECT_EQ(lsbBuilder.motionNotify(1, 0, 0, 0, 0, 0).size(), 32u);
    EXPECT_EQ(lsbBuilder.expose(1, 0, 0, 100, 100, 0, 0).size(), 32u);
    EXPECT_EQ(lsbBuilder.configureNotify(1, 0, 0, 100, 100, 0, 0).size(), 32u);
    EXPECT_EQ(lsbBuilder.destroyNotify(1, 0).size(), 32u);
}

TEST_F(EventBuilderTest, NegativeCoordinates) {
    auto evt = lsbBuilder.buttonPress(0x200001, -10, -20, 1, 1, 100);
    int16_t evtX = (int16_t)lsb.read16(evt.data(), 24);
    int16_t evtY = (int16_t)lsb.read16(evt.data(), 26);
    EXPECT_EQ(evtX, -10);
    EXPECT_EQ(evtY, -20);
}
