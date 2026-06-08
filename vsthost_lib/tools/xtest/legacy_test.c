/* Isolate the legacy glXChooseVisual + glXCreateContext path (what wine's GLX
 * init and glxgears use) and capture the exact failure. */
#include <X11/Xlib.h>
#include <GL/glx.h>
#include <stdio.h>

static int errHandler(Display* d, XErrorEvent* e) {
    char b[256];
    XGetErrorText(d, e->error_code, b, sizeof(b));
    fprintf(stderr, "X ERROR: code=%d (%s) major=%d minor=%d resource=0x%lx\n",
            e->error_code, b, e->request_code, e->minor_code, e->resourceid);
    return 0;
}

int main(void) {
    Display* d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    XSetErrorHandler(errHandler);

    int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, None };
    XVisualInfo* vis = glXChooseVisual(d, DefaultScreen(d), attr);
    if (!vis) { fprintf(stderr, "glXChooseVisual -> NULL\n"); return 1; }
    fprintf(stderr, "glXChooseVisual -> visualid=0x%lx depth=%d class=%d\n",
            vis->visualid, vis->depth, vis->class);

    GLXContext ctx = glXCreateContext(d, vis, NULL, True);
    fprintf(stderr, "glXCreateContext(direct=True) -> %p\n", (void*)ctx);
    if (!ctx) {
        ctx = glXCreateContext(d, vis, NULL, False);
        fprintf(stderr, "glXCreateContext(direct=False) -> %p\n", (void*)ctx);
    }
    XSync(d, False);
    if (ctx) fprintf(stderr, "SUCCESS, isDirect=%d\n", glXIsDirect(d, ctx));
    return ctx ? 0 : 2;
}
