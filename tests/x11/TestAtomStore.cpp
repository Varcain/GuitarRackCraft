#include <gtest/gtest.h>
#include "X11AtomStore.h"

using guitarrackcraft::X11AtomStore;

TEST(AtomStore, InternNewAtom) {
    X11AtomStore store;
    uint32_t id = store.intern("WM_NAME", false);
    EXPECT_NE(id, 0u);
}

TEST(AtomStore, InternSameAtomTwice) {
    X11AtomStore store;
    uint32_t id1 = store.intern("WM_NAME", false);
    uint32_t id2 = store.intern("WM_NAME", false);
    EXPECT_EQ(id1, id2);
}

TEST(AtomStore, InternDifferentAtoms) {
    X11AtomStore store;
    uint32_t id1 = store.intern("WM_NAME", false);
    uint32_t id2 = store.intern("WM_CLASS", false);
    EXPECT_NE(id1, id2);
}

TEST(AtomStore, OnlyIfExistsNotFound) {
    X11AtomStore store;
    uint32_t id = store.intern("NONEXISTENT", true);
    EXPECT_EQ(id, 0u);
}

TEST(AtomStore, OnlyIfExistsFound) {
    X11AtomStore store;
    uint32_t id1 = store.intern("WM_NAME", false);
    uint32_t id2 = store.intern("WM_NAME", true);
    EXPECT_EQ(id1, id2);
}

TEST(AtomStore, GetAtomNameSuccess) {
    X11AtomStore store;
    uint32_t id = store.intern("WM_PROTOCOLS", false);
    EXPECT_EQ(store.getName(id), "WM_PROTOCOLS");
}

TEST(AtomStore, GetAtomNameUnknown) {
    X11AtomStore store;
    EXPECT_EQ(store.getName(999), "");
}

TEST(AtomStore, ClearResetsState) {
    X11AtomStore store;
    uint32_t id = store.intern("WM_NAME", false);
    EXPECT_NE(id, 0u);
    store.clear();
    EXPECT_EQ(store.intern("WM_NAME", true), 0u);
    EXPECT_EQ(store.getName(id), "");
}

TEST(AtomStore, SequentialIds) {
    X11AtomStore store;
    uint32_t id1 = store.intern("ATOM_A", false);
    uint32_t id2 = store.intern("ATOM_B", false);
    uint32_t id3 = store.intern("ATOM_C", false);
    EXPECT_EQ(id1, 1u);
    EXPECT_EQ(id2, 2u);
    EXPECT_EQ(id3, 3u);
}

TEST(AtomStore, EmptyNameRejected) {
    X11AtomStore store;
    uint32_t id = store.intern("", false);
    EXPECT_EQ(id, 0u);
}

TEST(AtomStore, ManyAtoms) {
    X11AtomStore store;
    for (int i = 0; i < 1000; i++) {
        std::string name = "ATOM_" + std::to_string(i);
        uint32_t id = store.intern(name, false);
        EXPECT_NE(id, 0u);
        EXPECT_EQ(store.getName(id), name);
    }
    // Verify all still accessible
    for (int i = 0; i < 1000; i++) {
        std::string name = "ATOM_" + std::to_string(i);
        uint32_t id = store.intern(name, true);
        EXPECT_NE(id, 0u);
        EXPECT_EQ(store.getName(id), name);
    }
}

TEST(AtomStore, ClearThenReuse) {
    X11AtomStore store;
    store.intern("A", false);
    store.intern("B", false);
    store.clear();
    // After clear, IDs restart from 1
    uint32_t id = store.intern("C", false);
    EXPECT_EQ(id, 1u);
}
