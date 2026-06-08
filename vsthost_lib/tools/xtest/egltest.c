/* Native EGL-on-X11 render test (no wine) — isolates how Mesa's EGL X11
 * platform presents to our server. Clears BLUE and eglSwapBuffers. If our
 * framebuffer turns blue, EGL present works; if 0 PutImage, EGL uses a path
 * (DRI3/Present/kopper) our server doesn't yet support. */
#define EGL_EGLEXT_PROTOTYPES
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    Display* xd = XOpenDisplay(NULL);
    if (!xd) { fprintf(stderr, "no X display\n"); return 1; }

    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatDpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay ed = getPlatDpy ? getPlatDpy(EGL_PLATFORM_X11_KHR, xd, NULL)
                               : eglGetDisplay((EGLNativeDisplayType)xd);
    EGLint major, minor;
    if (!eglInitialize(ed, &major, &minor)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
    fprintf(stderr, "EGL %d.%d vendor=%s\n", major, minor, eglQueryString(ed, EGL_VENDOR));

    EGLint cfgAttr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(ed, cfgAttr, &cfg, 1, &n) || n == 0) { fprintf(stderr, "no EGL config\n"); return 1; }
    EGLint vid = 0;
    eglGetConfigAttrib(ed, cfg, EGL_NATIVE_VISUAL_ID, &vid);
    fprintf(stderr, "chose EGL config, native visual id=0x%x\n", vid);

    XVisualInfo vtmpl; vtmpl.visualid = vid; int vcount = 0;
    XVisualInfo* vi = XGetVisualInfo(xd, VisualIDMask, &vtmpl, &vcount);
    if (!vi) { fprintf(stderr, "XGetVisualInfo failed for 0x%x\n", vid); return 1; }

    XSetWindowAttributes swa;
    swa.colormap = XCreateColormap(xd, RootWindow(xd, vi->screen), vi->visual, AllocNone);
    swa.event_mask = ExposureMask;
    Window win = XCreateWindow(xd, RootWindow(xd, vi->screen), 0, 0, 400, 300, 0,
                               vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
    XMapWindow(xd, win);
    XStoreName(xd, win, "egltest");
    XSync(xd, False);

    eglBindAPI(EGL_OPENGL_API);
    EGLSurface surf = eglCreateWindowSurface(ed, cfg, (EGLNativeWindowType)win, NULL);
    if (surf == EGL_NO_SURFACE) { fprintf(stderr, "eglCreateWindowSurface failed: 0x%x\n", eglGetError()); return 1; }
    EGLContext ctx = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, NULL);
    if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext failed: 0x%x\n", eglGetError()); return 1; }
    if (!eglMakeCurrent(ed, surf, surf, ctx)) { fprintf(stderr, "eglMakeCurrent failed: 0x%x\n", eglGetError()); return 1; }
    fprintf(stderr, "GL_RENDERER=%s\n", (const char*)glGetString(GL_RENDERER));

    for (int i = 0; i < 12; i++) {
        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);   /* BLUE */
        glClear(GL_COLOR_BUFFER_BIT);
        if (!eglSwapBuffers(ed, surf)) fprintf(stderr, "eglSwapBuffers err 0x%x\n", eglGetError());
        usleep(120000);
    }
    fprintf(stderr, "swapped 12x blue, done\n");
    return 0;
}
