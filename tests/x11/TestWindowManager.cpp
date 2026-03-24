#include <gtest/gtest.h>
#include "X11WindowManager.h"

using namespace guitarrackcraft;

static constexpr uint32_t kRoot = 1;

class WindowManagerTest : public ::testing::Test {
protected:
    X11WindowManager wm{kRoot};
};

TEST_F(WindowManagerTest, CreateFirstChild) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    EXPECT_EQ(wm.childWindows().size(), 1u);
    EXPECT_EQ(wm.childWindows()[0], 0x200001u);
}

TEST_F(WindowManagerTest, CreateChildRecordsSize) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    auto [w, h] = wm.getSize(0x200001);
    EXPECT_EQ(w, 400);
    EXPECT_EQ(h, 300);
}

TEST_F(WindowManagerTest, CreateChildRecordsPosition) {
    wm.createWindow(0x200001, kRoot, 10, 20, 400, 300);
    auto pos = wm.getPosition(0x200001);
    EXPECT_EQ(pos.x, 10);
    EXPECT_EQ(pos.y, 20);
    EXPECT_EQ(pos.parent, kRoot);
}

TEST_F(WindowManagerTest, NewWindowIsUnmapped) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    EXPECT_TRUE(wm.isUnmapped(0x200001));
    EXPECT_FALSE(wm.isMapped(0x200001));
}

TEST_F(WindowManagerTest, MapWindowClearsUnmapped) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    wm.mapWindow(0x200001);
    EXPECT_FALSE(wm.isUnmapped(0x200001));
    EXPECT_TRUE(wm.isMapped(0x200001));
}

TEST_F(WindowManagerTest, UnmapWindowSetsUnmapped) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    wm.mapWindow(0x200001);
    wm.unmapWindow(0x200001);
    EXPECT_TRUE(wm.isUnmapped(0x200001));
}

TEST_F(WindowManagerTest, DestroyWindowCleansUp) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    wm.destroyWindow(0x200001);
    EXPECT_TRUE(wm.childWindows().empty());
    EXPECT_FALSE(wm.exists(0x200001));
    EXPECT_EQ(wm.getSize(0x200001), std::make_pair(0, 0));
}

TEST_F(WindowManagerTest, GetAbsolutePosSimple) {
    wm.createWindow(0x200001, kRoot, 10, 20, 400, 300);
    auto [ax, ay] = wm.getAbsolutePos(0x200001);
    EXPECT_EQ(ax, 10);
    EXPECT_EQ(ay, 20);
}

TEST_F(WindowManagerTest, GetAbsolutePosNested) {
    // Top-level child at (10, 20)
    wm.createWindow(0x200001, kRoot, 10, 20, 400, 300);
    // Grandchild at (5, 5) relative to top-level
    wm.createWindow(0x200002, 0x200001, 5, 5, 100, 50);
    // getAbsolutePos returns offset relative to top-level plugin window (childWindows[0]),
    // so it stops before adding the top-level window's own position
    auto [ax, ay] = wm.getAbsolutePos(0x200002);
    EXPECT_EQ(ax, 5);
    EXPECT_EQ(ay, 5);
}

TEST_F(WindowManagerTest, GetAbsolutePosStopsAtTopLevel) {
    wm.createWindow(0x200001, kRoot, 10, 20, 400, 300);
    wm.createWindow(0x200002, 0x200001, 5, 5, 100, 50);
    wm.createWindow(0x200003, 0x200002, 3, 3, 50, 25);
    // Should accumulate: 3+5 = 8, 3+5 = 8 (stops at 0x200001 which is childWindows[0])
    auto [ax, ay] = wm.getAbsolutePos(0x200003);
    EXPECT_EQ(ax, 8);
    EXPECT_EQ(ay, 8);
}

TEST_F(WindowManagerTest, HitTestTopLevel) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    wm.mapWindow(0x200001);
    auto hit = wm.hitTest(100, 100);
    EXPECT_EQ(hit.wid, 0x200001u);
}

TEST_F(WindowManagerTest, HitTestChildOverlap) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    wm.mapWindow(0x200001);
    // Two children overlapping at (50,50)
    wm.createWindow(0x200002, 0x200001, 0, 0, 200, 200);
    wm.mapWindow(0x200002);
    wm.createWindow(0x200003, 0x200001, 0, 0, 200, 200);
    wm.mapWindow(0x200003);
    // Last created (0x200003) is topmost
    auto hit = wm.hitTest(50, 50);
    EXPECT_EQ(hit.wid, 0x200003u);
}

TEST_F(WindowManagerTest, HitTestSkipsUnmapped) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    wm.mapWindow(0x200001);
    wm.createWindow(0x200002, 0x200001, 0, 0, 200, 200);
    // Don't map 0x200002 — it stays unmapped
    auto hit = wm.hitTest(50, 50);
    // Should fall through to top-level
    EXPECT_EQ(hit.wid, 0x200001u);
}

TEST_F(WindowManagerTest, HitTestLocalCoords) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    wm.mapWindow(0x200001);
    wm.createWindow(0x200002, 0x200001, 100, 50, 200, 200);
    wm.mapWindow(0x200002);
    auto hit = wm.hitTest(150, 100);
    EXPECT_EQ(hit.wid, 0x200002u);
    EXPECT_EQ(hit.localX, 50);
    EXPECT_EQ(hit.localY, 50);
}

TEST_F(WindowManagerTest, HitTestMiss) {
    wm.createWindow(0x200001, kRoot, 0, 0, 100, 100);
    wm.mapWindow(0x200001);
    wm.createWindow(0x200002, 0x200001, 200, 200, 50, 50);
    wm.mapWindow(0x200002);
    // Point at (10, 10) misses 0x200002 — returns top-level
    auto hit = wm.hitTest(10, 10);
    EXPECT_EQ(hit.wid, 0x200001u);
}

TEST_F(WindowManagerTest, ConfigureWindowUpdatesSize) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    wm.configureWindow(0x200001, 0, 0, 800, 600);
    auto [w, h] = wm.getSize(0x200001);
    EXPECT_EQ(w, 800);
    EXPECT_EQ(h, 600);
}

TEST_F(WindowManagerTest, ConfigureWindowUpdatesPosition) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    wm.configureWindow(0x200001, 50, 60, 400, 300);
    auto pos = wm.getPosition(0x200001);
    EXPECT_EQ(pos.x, 50);
    EXPECT_EQ(pos.y, 60);
}

TEST_F(WindowManagerTest, ClearResetsState) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    wm.createWindow(0x200002, 0x200001, 10, 10, 100, 100);
    wm.clear();
    EXPECT_TRUE(wm.childWindows().empty());
    EXPECT_FALSE(wm.exists(0x200001));
    EXPECT_FALSE(wm.exists(0x200002));
    EXPECT_EQ(wm.originalChildW(), 0);
    EXPECT_EQ(wm.originalChildH(), 0);
}

TEST_F(WindowManagerTest, OriginalChildSizeTracking) {
    wm.createWindow(0x200001, kRoot, 0, 0, 640, 480);
    EXPECT_EQ(wm.originalChildW(), 640);
    EXPECT_EQ(wm.originalChildH(), 480);
    // Second child doesn't change the original
    wm.createWindow(0x200002, 0x200001, 0, 0, 100, 50);
    EXPECT_EQ(wm.originalChildW(), 640);
    EXPECT_EQ(wm.originalChildH(), 480);
}

TEST_F(WindowManagerTest, EventMask) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    EXPECT_EQ(wm.getEventMask(0x200001), 0u);
    wm.setEventMask(0x200001, 0x00600000);
    EXPECT_EQ(wm.getEventMask(0x200001), 0x00600000u);
}

TEST_F(WindowManagerTest, MultipleChildren) {
    for (uint32_t i = 0; i < 10; i++) {
        wm.createWindow(0x200001 + i, kRoot, (int)(i * 50), 0, 40, 40);
    }
    EXPECT_EQ(wm.childWindows().size(), 10u);
    wm.destroyWindow(0x200005);
    EXPECT_EQ(wm.childWindows().size(), 9u);
    EXPECT_FALSE(wm.exists(0x200005));
}

TEST_F(WindowManagerTest, SetPositionX) {
    wm.createWindow(0x200001, kRoot, 10, 20, 100, 100);
    EXPECT_TRUE(wm.setPositionX(0x200001, 50));
    EXPECT_EQ(wm.getPosition(0x200001).x, 50);
    EXPECT_EQ(wm.getPosition(0x200001).y, 20);  // unchanged
    // No-op when same value
    EXPECT_FALSE(wm.setPositionX(0x200001, 50));
}

TEST_F(WindowManagerTest, SetPositionY) {
    wm.createWindow(0x200001, kRoot, 10, 20, 100, 100);
    EXPECT_TRUE(wm.setPositionY(0x200001, 99));
    EXPECT_EQ(wm.getPosition(0x200001).y, 99);
    EXPECT_EQ(wm.getPosition(0x200001).x, 10);  // unchanged
}

TEST_F(WindowManagerTest, SetSize) {
    wm.createWindow(0x200001, kRoot, 0, 0, 100, 100);
    wm.setSize(0x200001, 200, 300);
    auto [w, h] = wm.getSize(0x200001);
    EXPECT_EQ(w, 200);
    EXPECT_EQ(h, 300);
}

TEST_F(WindowManagerTest, RaiseSubtreeToFront) {
    // Create top-level + two children parented to root (popup-like)
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    wm.createWindow(0x200002, 0x200001, 10, 10, 50, 50);
    wm.createWindow(0x200003, kRoot, 0, 0, 100, 100);  // popup
    wm.createWindow(0x200004, 0x200003, 5, 5, 30, 30);  // popup child
    wm.createWindow(0x200005, 0x200001, 20, 20, 50, 50);

    // Order before raise: 1, 2, 3, 4, 5
    EXPECT_EQ(wm.childWindows().size(), 5u);

    // Raise popup subtree (3 + 4)
    size_t moved = wm.raiseSubtreeToFront(0x200003);
    EXPECT_EQ(moved, 2u);

    // 3 and 4 should now be at the end
    auto& cw = wm.childWindows();
    EXPECT_EQ(cw.back(), 0x200004u);
    EXPECT_EQ(cw[cw.size() - 2], 0x200003u);
}

TEST_F(WindowManagerTest, RaiseTopLevelIsNoOp) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    size_t moved = wm.raiseSubtreeToFront(0x200001);
    EXPECT_EQ(moved, 0u);  // top-level can't be raised
}

TEST_F(WindowManagerTest, GetMappedChildRectsOf) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    wm.mapWindow(0x200001);
    wm.createWindow(0x200002, 0x200001, 10, 20, 50, 60);
    wm.mapWindow(0x200002);
    wm.createWindow(0x200003, 0x200001, 100, 100, 30, 40);
    // 0x200003 is unmapped (default)

    auto rects = wm.getMappedChildRectsOf(0x200001);
    // Only 0x200002 is mapped
    EXPECT_EQ(rects.size(), 1u);
    EXPECT_EQ(rects[0].x1, 10);
    EXPECT_EQ(rects[0].y1, 20);
    EXPECT_EQ(rects[0].x2, 60);   // 10 + 50
    EXPECT_EQ(rects[0].y2, 80);   // 20 + 60
}

TEST_F(WindowManagerTest, GetMappedChildRectsEmpty) {
    wm.createWindow(0x200001, kRoot, 0, 0, 400, 300);
    auto rects = wm.getMappedChildRectsOf(0x200001);
    EXPECT_TRUE(rects.empty());
}
