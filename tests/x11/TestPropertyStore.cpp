#include <gtest/gtest.h>
#include "X11PropertyStore.h"

using namespace guitarrackcraft;

TEST(PropertyStore, ChangeAndGetReplace) {
    X11PropertyStore store;
    uint8_t data[] = "Hello";
    store.change(1, 100, 31/*STRING*/, 8, X11PropertyStore::ModeReplace, data, 5);

    uint32_t bytesAfter = 0;
    auto prop = store.get(1, 100, 0, 0, 100, bytesAfter);
    EXPECT_EQ(prop.type, 31u);
    EXPECT_EQ(prop.format, 8);
    ASSERT_EQ(prop.data.size(), 5u);
    EXPECT_EQ(std::string(prop.data.begin(), prop.data.end()), "Hello");
    EXPECT_EQ(bytesAfter, 0u);
}

TEST(PropertyStore, GetPropertyNotFound) {
    X11PropertyStore store;
    uint32_t bytesAfter = 0;
    auto prop = store.get(1, 999, 0, 0, 100, bytesAfter);
    EXPECT_EQ(prop.format, 0);
    EXPECT_TRUE(prop.data.empty());
}

TEST(PropertyStore, Format32RoundTrip) {
    X11PropertyStore store;
    uint32_t values[] = {0xAABBCCDD, 0x11223344};
    store.change(1, 200, 6/*CARDINAL*/, 32, X11PropertyStore::ModeReplace,
                 reinterpret_cast<const uint8_t*>(values), 2);

    uint32_t bytesAfter = 0;
    auto prop = store.get(1, 200, 0, 0, 100, bytesAfter);
    EXPECT_EQ(prop.format, 32);
    ASSERT_EQ(prop.data.size(), 8u);
    uint32_t got[2];
    memcpy(got, prop.data.data(), 8);
    EXPECT_EQ(got[0], 0xAABBCCDD);
    EXPECT_EQ(got[1], 0x11223344);
}

TEST(PropertyStore, Format16RoundTrip) {
    X11PropertyStore store;
    uint16_t values[] = {0x1234, 0x5678};
    store.change(1, 300, 6, 16, X11PropertyStore::ModeReplace,
                 reinterpret_cast<const uint8_t*>(values), 2);

    uint32_t bytesAfter = 0;
    auto prop = store.get(1, 300, 0, 0, 100, bytesAfter);
    EXPECT_EQ(prop.format, 16);
    ASSERT_EQ(prop.data.size(), 4u);
}

TEST(PropertyStore, TypeFilter) {
    X11PropertyStore store;
    uint8_t data[] = "test";
    store.change(1, 100, 31/*STRING*/, 8, X11PropertyStore::ModeReplace, data, 4);

    // Request with matching type
    uint32_t bytesAfter = 0;
    auto prop = store.get(1, 100, 31, 0, 100, bytesAfter);
    EXPECT_EQ(prop.format, 8);
    EXPECT_EQ(prop.data.size(), 4u);

    // Request with wrong type — returns type info but no data
    auto prop2 = store.get(1, 100, 99, 0, 100, bytesAfter);
    EXPECT_EQ(prop2.type, 31u);  // actual type
    EXPECT_EQ(prop2.format, 0);   // signals mismatch
    EXPECT_TRUE(prop2.data.empty());
    EXPECT_EQ(bytesAfter, 4u);    // bytes available
}

TEST(PropertyStore, PartialRead) {
    X11PropertyStore store;
    uint8_t data[16];
    memset(data, 0xAA, sizeof(data));
    store.change(1, 100, 31, 8, X11PropertyStore::ModeReplace, data, 16);

    // Read first 4 bytes (offset=0, maxLen=1 = 4 bytes)
    uint32_t bytesAfter = 0;
    auto prop = store.get(1, 100, 0, 0, 1, bytesAfter);
    EXPECT_EQ(prop.data.size(), 4u);
    EXPECT_EQ(bytesAfter, 12u);  // 16 - 4 = 12 remaining

    // Read from offset 1 (= 4 bytes in), maxLen=1 (= 4 bytes)
    auto prop2 = store.get(1, 100, 0, 1, 1, bytesAfter);
    EXPECT_EQ(prop2.data.size(), 4u);
    EXPECT_EQ(bytesAfter, 8u);   // 12 remaining - 4 returned = 8
}

TEST(PropertyStore, Prepend) {
    X11PropertyStore store;
    uint8_t initial[] = "World";
    store.change(1, 100, 31, 8, X11PropertyStore::ModeReplace, initial, 5);

    uint8_t prefix[] = "Hello";
    store.change(1, 100, 31, 8, X11PropertyStore::ModePrepend, prefix, 5);

    uint32_t bytesAfter = 0;
    auto prop = store.get(1, 100, 0, 0, 100, bytesAfter);
    ASSERT_EQ(prop.data.size(), 10u);
    EXPECT_EQ(std::string(prop.data.begin(), prop.data.end()), "HelloWorld");
}

TEST(PropertyStore, Append) {
    X11PropertyStore store;
    uint8_t initial[] = "Hello";
    store.change(1, 100, 31, 8, X11PropertyStore::ModeReplace, initial, 5);

    uint8_t suffix[] = "World";
    store.change(1, 100, 31, 8, X11PropertyStore::ModeAppend, suffix, 5);

    uint32_t bytesAfter = 0;
    auto prop = store.get(1, 100, 0, 0, 100, bytesAfter);
    ASSERT_EQ(prop.data.size(), 10u);
    EXPECT_EQ(std::string(prop.data.begin(), prop.data.end()), "HelloWorld");
}

TEST(PropertyStore, DeleteProperty) {
    X11PropertyStore store;
    uint8_t data[] = "test";
    store.change(1, 100, 31, 8, X11PropertyStore::ModeReplace, data, 4);
    store.remove(1, 100);

    uint32_t bytesAfter = 0;
    auto prop = store.get(1, 100, 0, 0, 100, bytesAfter);
    EXPECT_EQ(prop.format, 0);
    EXPECT_TRUE(prop.data.empty());
}

TEST(PropertyStore, ListProperties) {
    X11PropertyStore store;
    uint8_t data[] = "x";
    store.change(1, 100, 31, 8, X11PropertyStore::ModeReplace, data, 1);
    store.change(1, 200, 31, 8, X11PropertyStore::ModeReplace, data, 1);
    store.change(1, 300, 31, 8, X11PropertyStore::ModeReplace, data, 1);

    auto atoms = store.list(1);
    ASSERT_EQ(atoms.size(), 3u);
    // Sorted
    EXPECT_EQ(atoms[0], 100u);
    EXPECT_EQ(atoms[1], 200u);
    EXPECT_EQ(atoms[2], 300u);
}

TEST(PropertyStore, WindowIsolation) {
    X11PropertyStore store;
    uint8_t data1[] = "win1";
    uint8_t data2[] = "win2";
    store.change(1, 100, 31, 8, X11PropertyStore::ModeReplace, data1, 4);
    store.change(2, 100, 31, 8, X11PropertyStore::ModeReplace, data2, 4);

    uint32_t bytesAfter = 0;
    auto prop1 = store.get(1, 100, 0, 0, 100, bytesAfter);
    auto prop2 = store.get(2, 100, 0, 0, 100, bytesAfter);
    EXPECT_EQ(std::string(prop1.data.begin(), prop1.data.end()), "win1");
    EXPECT_EQ(std::string(prop2.data.begin(), prop2.data.end()), "win2");
}

TEST(PropertyStore, ClearWindow) {
    X11PropertyStore store;
    uint8_t data[] = "x";
    store.change(1, 100, 31, 8, X11PropertyStore::ModeReplace, data, 1);
    store.change(1, 200, 31, 8, X11PropertyStore::ModeReplace, data, 1);
    store.change(2, 100, 31, 8, X11PropertyStore::ModeReplace, data, 1);

    store.clearWindow(1);
    EXPECT_TRUE(store.list(1).empty());
    EXPECT_EQ(store.list(2).size(), 1u);  // window 2 untouched
}

TEST(PropertyStore, ClearAll) {
    X11PropertyStore store;
    uint8_t data[] = "x";
    store.change(1, 100, 31, 8, X11PropertyStore::ModeReplace, data, 1);
    store.change(2, 200, 31, 8, X11PropertyStore::ModeReplace, data, 1);
    store.clear();
    EXPECT_TRUE(store.list(1).empty());
    EXPECT_TRUE(store.list(2).empty());
}
