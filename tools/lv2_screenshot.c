/*
 * lv2_screenshot.c — Minimal LV2 X11 UI host for screenshot capture.
 *
 * Opens an LV2 plugin UI, renders it, captures the child window to PPM.
 *
 * Usage: lv2_screenshot <ui_so> <plugin_uri> <ui_uri> <bundle_path> <output.ppm>
 * Build: gcc -O2 -o lv2_screenshot lv2_screenshot.c $(pkg-config --cflags --libs x11 lv2) -ldl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <lv2/ui/ui.h>

static void stub_write(LV2UI_Controller c, uint32_t pi, uint32_t bs,
                       uint32_t pp, const void *b) {
    (void)c; (void)pi; (void)bs; (void)pp; (void)b;
}

typedef struct { int w, h; } ReqSize;
static int resize_cb(LV2UI_Feature_Handle h, int w, int ht) {
    ReqSize *rs = (ReqSize *)h; rs->w = w; rs->h = ht; return 0;
}

static int save_ppm(const char *path, XImage *img) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", img->width, img->height);
    for (int y = 0; y < img->height; y++) {
        for (int x = 0; x < img->width; x++) {
            unsigned long px = XGetPixel(img, x, y);
            unsigned char rgb[3] = {
                (px >> 16) & 0xff, (px >> 8) & 0xff, px & 0xff
            };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return 0;
}

/* Recursively find the deepest window with actual content */
static Window find_content_window(Display *dpy, Window win, int depth) {
    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren = 0;
    if (!XQueryTree(dpy, win, &root_ret, &parent_ret, &children, &nchildren))
        return win;

    Window best = win;
    int best_area = 0;

    for (unsigned int c = 0; c < nchildren; c++) {
        XWindowAttributes cwa;
        if (XGetWindowAttributes(dpy, children[c], &cwa)) {
            int area = cwa.width * cwa.height;
            if (area > best_area && cwa.map_state == IsViewable) {
                best_area = area;
                best = children[c];
            }
        }
    }
    if (children) XFree(children);

    /* Recurse one level deeper if we found a child */
    if (best != win && depth < 3) {
        Window deeper = find_content_window(dpy, best, depth + 1);
        if (deeper != best) return deeper;
    }
    return best;
}

/* Composite all child windows onto a pixmap for proper capture */
static XImage *capture_composited(Display *dpy, Window top, int w, int h) {
    int screen = DefaultScreen(dpy);
    Pixmap pix = XCreatePixmap(dpy, top, w, h, DefaultDepth(dpy, screen));
    GC gc = XCreateGC(dpy, pix, 0, NULL);

    /* Fill with black */
    XSetForeground(dpy, gc, BlackPixel(dpy, screen));
    XFillRectangle(dpy, pix, gc, 0, 0, w, h);

    /* Copy top window content */
    XCopyArea(dpy, top, pix, gc, 0, 0, w, h, 0, 0);

    /* Copy each child window */
    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren = 0;
    if (XQueryTree(dpy, top, &root_ret, &parent_ret, &children, &nchildren)) {
        for (unsigned int c = 0; c < nchildren; c++) {
            XWindowAttributes cwa;
            if (XGetWindowAttributes(dpy, children[c], &cwa) &&
                cwa.map_state == IsViewable) {
                XCopyArea(dpy, children[c], pix, gc,
                          0, 0, cwa.width, cwa.height, cwa.x, cwa.y);

                /* Also copy grandchildren */
                Window gr, gp, *gchildren = NULL;
                unsigned int gn = 0;
                if (XQueryTree(dpy, children[c], &gr, &gp, &gchildren, &gn)) {
                    for (unsigned int g = 0; g < gn; g++) {
                        XWindowAttributes gwa;
                        if (XGetWindowAttributes(dpy, gchildren[g], &gwa) &&
                            gwa.map_state == IsViewable) {
                            XCopyArea(dpy, gchildren[g], pix, gc,
                                      0, 0, gwa.width, gwa.height,
                                      cwa.x + gwa.x, cwa.y + gwa.y);
                        }
                    }
                    if (gchildren) XFree(gchildren);
                }
            }
        }
        if (children) XFree(children);
    }

    XSync(dpy, False);
    XImage *img = XGetImage(dpy, pix, 0, 0, w, h, AllPlanes, ZPixmap);
    XFreePixmap(dpy, pix);
    XFreeGC(dpy, gc);
    return img;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "Usage: %s <ui.so> <plugin_uri> <ui_uri> <bundle> <output.ppm>\n", argv[0]);
        return 1;
    }
    const char *ui_so_path  = argv[1];
    const char *plugin_uri  = argv[2];
    const char *ui_uri      = argv[3];
    const char *bundle_path = argv[4];
    const char *output_path = argv[5];

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "Cannot open X display\n"); return 1; }
    int screen = DefaultScreen(dpy);

    Window parent = XCreateSimpleWindow(dpy, RootWindow(dpy, screen),
                                        0, 0, 800, 600, 0,
                                        BlackPixel(dpy, screen),
                                        BlackPixel(dpy, screen));
    XSelectInput(dpy, parent, ExposureMask | StructureNotifyMask | SubstructureNotifyMask);
    XMapWindow(dpy, parent);
    XFlush(dpy);

    void *handle = dlopen(ui_so_path, RTLD_NOW);
    if (!handle) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    typedef const LV2UI_Descriptor *(*DescFunc)(uint32_t);
    DescFunc desc_func = (DescFunc)dlsym(handle, "lv2ui_descriptor");
    if (!desc_func) { fprintf(stderr, "No lv2ui_descriptor\n"); return 1; }

    const LV2UI_Descriptor *desc = NULL;
    for (uint32_t i = 0; ; i++) {
        const LV2UI_Descriptor *d = desc_func(i);
        if (!d) break;
        if (strcmp(d->URI, ui_uri) == 0) { desc = d; break; }
    }
    if (!desc) desc = desc_func(0);
    if (!desc) { fprintf(stderr, "No UI descriptor\n"); return 1; }

    ReqSize req = { 800, 600 };
    LV2UI_Resize resize_feat = { &req, resize_cb };
    LV2_Feature parent_feature = { LV2_UI__parent, (void *)parent };
    LV2_Feature resize_feature = { LV2_UI__resize, &resize_feat };
    const LV2_Feature *features[] = { &parent_feature, &resize_feature, NULL };

    LV2UI_Widget widget = NULL;
    LV2UI_Handle ui = desc->instantiate(desc, plugin_uri, bundle_path,
                                         stub_write, NULL, &widget, features);
    if (!ui) { fprintf(stderr, "UI instantiate failed\n"); dlclose(handle); return 1; }

    if (req.w > 0 && req.h > 0)
        XResizeWindow(dpy, parent, req.w, req.h);

    /* Force-map all child windows created by the plugin */
    XMapSubwindows(dpy, parent);
    XFlush(dpy);

    /* Initial event processing */
    for (int i = 0; i < 30; i++) {
        /* Keep mapping any new subwindows that appear */
        XMapSubwindows(dpy, parent);
        while (XPending(dpy)) { XEvent ev; XNextEvent(dpy, &ev); }
        usleep(20000);
    }

    /* Map any grandchild windows too */
    {
        Window root_ret, parent_ret, *children = NULL;
        unsigned int nchildren = 0;
        XQueryTree(dpy, parent, &root_ret, &parent_ret, &children, &nchildren);
        for (unsigned int c = 0; c < nchildren; c++)
            XMapSubwindows(dpy, children[c]);
        if (children) XFree(children);
        XFlush(dpy);
    }

    /* Get the idle interface — this is critical for xputty-based UIs */
    const LV2UI_Idle_Interface *idle_iface = NULL;
    if (desc->extension_data)
        idle_iface = (const LV2UI_Idle_Interface *)desc->extension_data(LV2_UI__idleInterface);

    /* Send port events to trigger control redraws */
    if (desc->port_event) {
        float bypass = 0.0f;
        desc->port_event(ui, 0, sizeof(float), 0, &bypass);
        for (uint32_t p = 1; p < 20; p++) {
            float val = 0.5f;
            desc->port_event(ui, p, sizeof(float), 0, &val);
        }
    }

    /* Pump the idle interface + X events to let the UI fully render */
    for (int i = 0; i < 120; i++) {
        if (idle_iface && idle_iface->idle)
            idle_iface->idle(ui);
        while (XPending(dpy)) { XEvent ev; XNextEvent(dpy, &ev); }
        usleep(16000);  /* ~60fps */
    }

    /* Try capturing child windows directly, then fall back to compositing */
    XImage *img = NULL;
    int cap_w = 0, cap_h = 0;
    {
        Window root_ret, parent_ret, *children = NULL;
        unsigned int nchildren = 0;
        XQueryTree(dpy, parent, &root_ret, &parent_ret, &children, &nchildren);

        for (unsigned int c = 0; c < nchildren && !img; c++) {
            XWindowAttributes cwa;
            if (XGetWindowAttributes(dpy, children[c], &cwa) &&
                cwa.width > 10 && cwa.height > 10 && cwa.map_state == IsViewable) {
                img = XGetImage(dpy, children[c], 0, 0, cwa.width, cwa.height,
                                AllPlanes, ZPixmap);
                cap_w = cwa.width; cap_h = cwa.height;
            }
        }
        if (children) XFree(children);
    }

    /* Fall back to compositing if no child captured */
    if (!img) {
        XWindowAttributes pwa;
        XGetWindowAttributes(dpy, parent, &pwa);
        img = capture_composited(dpy, parent, pwa.width, pwa.height);
        if (img) { cap_w = img->width; cap_h = img->height; }
    }

    if (!img) {
        fprintf(stderr, "Capture failed\n");
    } else {
        save_ppm(output_path, img);
        fprintf(stdout, "%dx%d\n", cap_w, cap_h);
        XDestroyImage(img);
    }

    desc->cleanup(ui);
    dlclose(handle);
    XDestroyWindow(dpy, parent);
    XCloseDisplay(dpy);
    return 0;
}
