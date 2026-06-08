/* Minimal modern-GLX render+present test against our X server.
 * Uses glXChooseFBConfig + glXCreateContextAttribsARB (the path wine uses),
 * clears the window RED and SwapBuffers a few times. If our framebuffer turns
 * red, the GL present path (drisw -> XPutImage) works end to end. */
#include <X11/Xlib.h>
#include <GL/glx.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef GLXContext (*CCA)(Display*, GLXFBConfig, GLXContext, Bool, const int*);

int main(void) {
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "XOpenDisplay failed\n"); return 1; }
    int scr = DefaultScreen(dpy);

    int attribs[] = {
        GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, GLX_DEPTH_SIZE, 24,
        GLX_DOUBLEBUFFER, True, None
    };
    int n = 0;
    GLXFBConfig* fbc = glXChooseFBConfig(dpy, scr, attribs, &n);
    fprintf(stderr, "glXChooseFBConfig -> %d configs\n", n);
    if (!fbc || n == 0) { fprintf(stderr, "no fbconfig\n"); return 1; }

    XVisualInfo* vi = glXGetVisualFromFBConfig(dpy, fbc[0]);
    fprintf(stderr, "visual=%p id=0x%lx depth=%d\n", (void*)vi, vi ? vi->visualid : 0, vi ? vi->depth : 0);
    if (!vi) return 1;

    XSetWindowAttributes swa;
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, scr), vi->visual, AllocNone);
    swa.event_mask = ExposureMask | StructureNotifyMask;
    Window win = XCreateWindow(dpy, RootWindow(dpy, scr), 0, 0, 400, 300, 0,
                               vi->depth, InputOutput, vi->visual,
                               CWColormap | CWEventMask, &swa);
    XMapWindow(dpy, win);
    XStoreName(dpy, win, "gltest");

    CCA createCtx = (CCA)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
    fprintf(stderr, "glXCreateContextAttribsARB=%p\n", (void*)createCtx);
    int ctxattr[] = { 0x2091 /*MAJOR*/, 2, 0x2092 /*MINOR*/, 0, None };
    GLXContext ctx = createCtx ? createCtx(dpy, fbc[0], 0, True, ctxattr)
                               : glXCreateNewContext(dpy, fbc[0], GLX_RGBA_TYPE, 0, True);
    fprintf(stderr, "ctx=%p\n", (void*)ctx);
    if (!ctx) { fprintf(stderr, "context creation failed\n"); return 1; }

    if (!glXMakeCurrent(dpy, win, ctx)) { fprintf(stderr, "glXMakeCurrent failed\n"); return 1; }
    fprintf(stderr, "GL_RENDERER=%s\n", (const char*)glGetString(GL_RENDERER));
    fprintf(stderr, "GL_VERSION=%s\n", (const char*)glGetString(GL_VERSION));

    for (int i = 0; i < 12; i++) {
        glClearColor(1.0f, 0.0f, 0.0f, 1.0f);   /* RED */
        glClear(GL_COLOR_BUFFER_BIT);
        glXSwapBuffers(dpy, win);
        usleep(120000);
    }
    fprintf(stderr, "swapped 12x red, done\n");
    XFlush(dpy);
    sleep(1);
    return 0;
}
