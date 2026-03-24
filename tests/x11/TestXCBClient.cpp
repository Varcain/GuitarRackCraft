#include <gtest/gtest.h>
#include "TestHelpers.h"
#include <xcb/xcb.h>
#include <unistd.h>
#include <cstring>
#include <string>

using namespace guitarrackcraft;
using namespace guitarrackcraft::test;

static constexpr int kTestDisplayNum = 97;

class XCBClientTest : public ::testing::Test {
protected:
    X11TestServer server;
    xcb_connection_t* conn = nullptr;
    xcb_screen_t* screen = nullptr;

    void SetUp() override {
        int port = server.startTCP(kTestDisplayNum, 800, 600);
        ASSERT_GT(port, 0) << "Failed to start TCP server";

        // Give server thread a moment to enter accept()
        usleep(50000);

        std::string display = "127.0.0.1:" + std::to_string(kTestDisplayNum);
        int screenNum = 0;
        conn = xcb_connect(display.c_str(), &screenNum);
        ASSERT_NE(conn, nullptr);
        ASSERT_EQ(xcb_connection_has_error(conn), 0)
            << "xcb_connect failed for " << display;

        const xcb_setup_t* setup = xcb_get_setup(conn);
        ASSERT_NE(setup, nullptr);
        xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
        ASSERT_GT(iter.rem, 0);
        screen = iter.data;
        ASSERT_NE(screen, nullptr);
    }

    void TearDown() override {
        if (conn) xcb_disconnect(conn);
        server.stop();
    }
};

TEST_F(XCBClientTest, ConnectSucceeds) {
    EXPECT_NE(screen->root, 0u);
    EXPECT_EQ(screen->width_in_pixels, 800);
    EXPECT_EQ(screen->height_in_pixels, 600);
    EXPECT_EQ(screen->root_depth, 24);
}

TEST_F(XCBClientTest, InternAtom) {
    const char* name = "WM_NAME";
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, 0, strlen(name), name);
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(conn, cookie, nullptr);
    ASSERT_NE(reply, nullptr);
    EXPECT_NE(reply->atom, 0u);
    free(reply);
}

TEST_F(XCBClientTest, InternAtomRoundTrip) {
    const char* name = "MY_XCB_TEST_ATOM";
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, 0, strlen(name), name);
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(conn, cookie, nullptr);
    ASSERT_NE(reply, nullptr);
    xcb_atom_t atom = reply->atom;
    EXPECT_NE(atom, 0u);
    free(reply);

    // Reverse lookup
    xcb_get_atom_name_cookie_t nameCookie = xcb_get_atom_name(conn, atom);
    xcb_get_atom_name_reply_t* nameReply = xcb_get_atom_name_reply(conn, nameCookie, nullptr);
    ASSERT_NE(nameReply, nullptr);
    int nameLen = xcb_get_atom_name_name_length(nameReply);
    EXPECT_EQ(nameLen, (int)strlen(name));
    std::string got(xcb_get_atom_name_name(nameReply), nameLen);
    EXPECT_EQ(got, name);
    free(nameReply);
}

TEST_F(XCBClientTest, CreateWindowAndGetGeometry) {
    xcb_window_t wid = xcb_generate_id(conn);
    xcb_create_window(conn, 24, wid, screen->root,
                      10, 20, 400, 300, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, nullptr);
    xcb_flush(conn);

    // Consume the Expose event the server sends
    xcb_generic_event_t* evt = xcb_wait_for_event(conn);
    ASSERT_NE(evt, nullptr);
    EXPECT_EQ(evt->response_type & 0x7F, 12);  // Expose
    free(evt);

    xcb_get_geometry_cookie_t geomCookie = xcb_get_geometry(conn, wid);
    xcb_get_geometry_reply_t* geom = xcb_get_geometry_reply(conn, geomCookie, nullptr);
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->x, 10);
    EXPECT_EQ(geom->y, 20);
    EXPECT_EQ(geom->width, 400);
    EXPECT_EQ(geom->height, 300);
    free(geom);
}

TEST_F(XCBClientTest, MapWindow) {
    xcb_window_t wid = xcb_generate_id(conn);
    xcb_create_window(conn, 24, wid, screen->root,
                      0, 0, 200, 150, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, nullptr);
    xcb_flush(conn);
    xcb_generic_event_t* evt1 = xcb_wait_for_event(conn);
    free(evt1);  // Expose from create

    xcb_map_window(conn, wid);
    xcb_flush(conn);
    xcb_generic_event_t* evt2 = xcb_wait_for_event(conn);
    ASSERT_NE(evt2, nullptr);
    EXPECT_EQ(evt2->response_type & 0x7F, 12);  // Expose from map
    free(evt2);

    xcb_get_window_attributes_cookie_t attrCookie = xcb_get_window_attributes(conn, wid);
    xcb_get_window_attributes_reply_t* attr = xcb_get_window_attributes_reply(conn, attrCookie, nullptr);
    ASSERT_NE(attr, nullptr);
    EXPECT_EQ(attr->map_state, XCB_MAP_STATE_VIEWABLE);
    free(attr);
}

TEST_F(XCBClientTest, QueryExtension) {
    const char* name = "GLX";
    xcb_query_extension_cookie_t cookie = xcb_query_extension(conn, strlen(name), name);
    xcb_query_extension_reply_t* reply = xcb_query_extension_reply(conn, cookie, nullptr);
    ASSERT_NE(reply, nullptr);
    EXPECT_EQ(reply->present, 1);
    EXPECT_EQ(reply->major_opcode, 128);
    free(reply);
}

TEST_F(XCBClientTest, QueryPointer) {
    xcb_query_pointer_cookie_t cookie = xcb_query_pointer(conn, screen->root);
    xcb_query_pointer_reply_t* reply = xcb_query_pointer_reply(conn, cookie, nullptr);
    ASSERT_NE(reply, nullptr);
    EXPECT_EQ(reply->same_screen, 1);
    EXPECT_EQ(reply->root, screen->root);
    free(reply);
}

TEST_F(XCBClientTest, ListExtensions) {
    xcb_list_extensions_cookie_t cookie = xcb_list_extensions(conn);
    xcb_list_extensions_reply_t* reply = xcb_list_extensions_reply(conn, cookie, nullptr);
    ASSERT_NE(reply, nullptr);
    EXPECT_GE(reply->names_len, 1);

    bool foundGLX = false;
    xcb_str_iterator_t iter = xcb_list_extensions_names_iterator(reply);
    while (iter.rem > 0) {
        std::string name(xcb_str_name(iter.data), xcb_str_name_length(iter.data));
        if (name == "GLX") foundGLX = true;
        xcb_str_next(&iter);
    }
    EXPECT_TRUE(foundGLX);
    free(reply);
}

TEST_F(XCBClientTest, PipelinedRequests) {
    // Send 10 InternAtom requests without waiting for replies
    std::vector<xcb_intern_atom_cookie_t> cookies;
    for (int i = 0; i < 10; i++) {
        std::string name = "PIPELINE_ATOM_" + std::to_string(i);
        cookies.push_back(xcb_intern_atom(conn, 0, name.size(), name.c_str()));
    }
    xcb_flush(conn);

    // Collect all replies
    std::vector<xcb_atom_t> atoms;
    for (auto& cookie : cookies) {
        xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(conn, cookie, nullptr);
        ASSERT_NE(reply, nullptr);
        EXPECT_NE(reply->atom, 0u);
        atoms.push_back(reply->atom);
        free(reply);
    }

    // All should be distinct
    for (size_t i = 0; i < atoms.size(); i++) {
        for (size_t j = i + 1; j < atoms.size(); j++) {
            EXPECT_NE(atoms[i], atoms[j]) << "Atoms " << i << " and " << j << " collide";
        }
    }
}
