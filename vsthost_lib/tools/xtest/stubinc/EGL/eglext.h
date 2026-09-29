/* Desktop stub for <EGL/eglext.h>
 * Only the extension types / constants AhbTexture.cpp references; its entry
 * points are looked up with eglGetProcAddress, which the stub answers with
 * null, so the AHB import path reports "unsupported" in the harness. */
#ifndef XTEST_STUB_EGL_EGLEXT_H
#define XTEST_STUB_EGL_EGLEXT_H

#include <EGL/egl.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AHardwareBuffer;

typedef void* EGLImageKHR;
typedef void* EGLSyncKHR;

#define EGL_NO_IMAGE_KHR                 ((EGLImageKHR)0)
#define EGL_NO_SYNC_KHR                  ((EGLSyncKHR)0)
#define EGL_IMAGE_PRESERVED_KHR          0x30D2
#define EGL_NATIVE_BUFFER_ANDROID        0x3140
#define EGL_SYNC_NATIVE_FENCE_ANDROID    0x3144
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID 0x3145

typedef EGLClientBuffer (*PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)(const struct AHardwareBuffer* buffer);
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                                                EGLClientBuffer buffer, const EGLint* attrib_list);
typedef EGLBoolean (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay dpy, EGLImageKHR image);
typedef EGLSyncKHR (*PFNEGLCREATESYNCKHRPROC)(EGLDisplay dpy, EGLenum type, const EGLint* attrib_list);
typedef EGLBoolean (*PFNEGLDESTROYSYNCKHRPROC)(EGLDisplay dpy, EGLSyncKHR sync);
typedef EGLint (*PFNEGLWAITSYNCKHRPROC)(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags);

#ifdef __cplusplus
}
#endif

#endif /* XTEST_STUB_EGL_EGLEXT_H */
