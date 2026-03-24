#include <gtest/gtest.h>
#include "X11PixmapStore.h"

using namespace guitarrackcraft;

TEST(PixmapStore, CreateAndGet) {
    X11PixmapStore store;
    store.create(100, 64, 32);
    auto* pm = store.get(100);
    ASSERT_NE(pm, nullptr);
    EXPECT_EQ(pm->w, 64);
    EXPECT_EQ(pm->h, 32);
    EXPECT_EQ(pm->pixels.size(), 64u * 32u);
}

TEST(PixmapStore, CreateFillsBackground) {
    X11PixmapStore store;
    store.create(100, 4, 4, 0xFFAABBCC);
    auto* pm = store.get(100);
    ASSERT_NE(pm, nullptr);
    for (auto pixel : pm->pixels) {
        EXPECT_EQ(pixel, 0xFFAABBCC);
    }
}

TEST(PixmapStore, DestroyRemoves) {
    X11PixmapStore store;
    store.create(100, 10, 10);
    EXPECT_TRUE(store.exists(100));
    store.destroy(100);
    EXPECT_FALSE(store.exists(100));
    EXPECT_EQ(store.get(100), nullptr);
}

TEST(PixmapStore, MultiplePixmaps) {
    X11PixmapStore store;
    store.create(1, 10, 10);
    store.create(2, 20, 20);
    store.create(3, 30, 30);
    EXPECT_TRUE(store.exists(1));
    EXPECT_TRUE(store.exists(2));
    EXPECT_TRUE(store.exists(3));
    EXPECT_EQ(store.get(1)->w, 10);
    EXPECT_EQ(store.get(2)->w, 20);
    EXPECT_EQ(store.get(3)->w, 30);
}

TEST(PixmapStore, GetNonExistent) {
    X11PixmapStore store;
    EXPECT_EQ(store.get(999), nullptr);
    EXPECT_FALSE(store.exists(999));
}

TEST(PixmapStore, ClearRemovesAll) {
    X11PixmapStore store;
    store.create(1, 10, 10);
    store.create(2, 20, 20);
    store.clear();
    EXPECT_FALSE(store.exists(1));
    EXPECT_FALSE(store.exists(2));
}

TEST(PixmapStore, ConstGet) {
    X11PixmapStore store;
    store.create(100, 8, 8);
    const X11PixmapStore& cstore = store;
    const PixmapData* pm = cstore.get(100);
    ASSERT_NE(pm, nullptr);
    EXPECT_EQ(pm->w, 8);
}

TEST(PixmapStore, PixmapPixelsAreWritable) {
    X11PixmapStore store;
    store.create(100, 4, 4, 0);
    auto* pm = store.get(100);
    pm->pixels[0] = 0xFFFF0000;
    EXPECT_EQ(store.get(100)->pixels[0], 0xFFFF0000u);
}
