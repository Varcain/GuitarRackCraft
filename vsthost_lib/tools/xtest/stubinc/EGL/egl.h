/* Desktop stub for <EGL/egl.h>
 * Android-flavoured: EGLNativeWindowType is ANativeWindow* so
 * eglCreateWindowSurface(display, config, ANativeWindow*, ...) type-checks.
 * None of these are ever called (attachSurface is never invoked). */
#ifndef XTEST_STUB_EGL_H
#define XTEST_STUB_EGL_H

#include <android/native_window.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef void* EGLConfig;
typedef void* EGLContext;
typedef void* EGLDisplay;
typedef void* EGLSurface;
typedef void* EGLClientBuffer;
typedef ANativeWindow* EGLNativeWindowType;
typedef void* EGLNativeDisplayType;
typedef void* EGLNativePixmapType;

#define EGL_FALSE 0
#define EGL_TRUE  1

#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NO_DISPLAY      ((EGLDisplay)0)
#define EGL_NO_CONTEXT      ((EGLContext)0)
#define EGL_NO_SURFACE      ((EGLSurface)0)

#define EGL_NONE                 0x3038
#define EGL_SURFACE_TYPE         0x3033
#define EGL_WINDOW_BIT           0x0004
#define EGL_RENDERABLE_TYPE      0x3040
#define EGL_OPENGL_ES2_BIT       0x0004
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id);
EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor);
EGLBoolean eglTerminate(EGLDisplay dpy);
EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint* attrib_list,
                           EGLConfig* configs, EGLint config_size,
                           EGLint* num_config);
EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                  EGLNativeWindowType win,
                                  const EGLint* attrib_list);
EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface);
EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config,
                            EGLContext share_context,
                            const EGLint* attrib_list);
EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx);
EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw,
                          EGLSurface read, EGLContext ctx);
EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface);

#ifdef __cplusplus
}
#endif

#endif /* XTEST_STUB_EGL_H */
