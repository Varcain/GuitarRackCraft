/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

// EGL+GLES2 backend for Pugl on Android/X11.
// Renders via EGL pbuffer (GPU-accelerated), then blits to X11 via XPutImage.
//
// Replaces the GLX backend (x11_gl.c) when building for Android, where Mesa
// software rasterization is prohibitively slow but EGL_DEFAULT_DISPLAY gives
// direct GPU access independent of the X11 server.

#include "../pugl-upstream/src/stub.h"
#include "../pugl-upstream/src/x11.h"

// Prevent pugl/gl.h from including GL/gl.h — we use GLES2/gl2.h instead
// (already included via our GL/gl.h shim from OpenGL-include.hpp)
#define PUGL_NO_INCLUDE_GL_H
#include "pugl/gl.h"
#include "pugl/pugl.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/log.h>
#define PUGL_LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "PuglEGL", __VA_ARGS__)
#define PUGL_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PuglEGL", __VA_ARGS__)
#else
#define PUGL_LOGD(...) do {} while(0)
#define PUGL_LOGE(...) do {} while(0)
#endif

typedef struct {
    EGLDisplay eglDisplay;
    EGLConfig  eglConfig;
    EGLContext eglContext;
    EGLSurface eglSurface;
    GC         gc;          // X11 Graphics Context for XPutImage
    XImage*    ximage;      // Reusable XImage wrapper (points to xputBuf)
    uint8_t*   readBuf;     // glReadPixels output (RGBA)
    uint8_t*   xputBuf;     // XPutImage input (BGRA, vertically flipped)
    int        bufW, bufH;
} PuglX11EglSurface;

static int
puglX11EglGetAttrib(const EGLDisplay display,
                    const EGLConfig  config,
                    const EGLint     attrib)
{
    EGLint value = 0;
    eglGetConfigAttrib(display, config, attrib, &value);
    return value;
}

// ---------------------------------------------------------------------------
// Resize helper — recreate EGL pbuffer + pixel buffers + XImage
//
// Called from enter() when window dimensions change. The EGL context is
// preserved; only the pbuffer surface and pixel buffers are recreated.

static PuglStatus
puglX11EglResizeBuffers(PuglView* view, int newW, int newH)
{
    PuglX11EglSurface* const surface =
        (PuglX11EglSurface*)view->impl->surface;
    PuglInternals* const impl    = view->impl;
    Display* const       display = view->world->impl->display;

    if (newW <= 0 || newH <= 0) return PUGL_SUCCESS;
    if (newW == surface->bufW && newH == surface->bufH) return PUGL_SUCCESS;

    PUGL_LOGD("puglX11EglResizeBuffers: %dx%d -> %dx%d",
        surface->bufW, surface->bufH, newW, newH);

    // Unbind context before destroying surface
    eglMakeCurrent(surface->eglDisplay, EGL_NO_SURFACE,
                   EGL_NO_SURFACE, EGL_NO_CONTEXT);

    // Destroy old pbuffer
    if (surface->eglSurface != EGL_NO_SURFACE) {
        eglDestroySurface(surface->eglDisplay, surface->eglSurface);
        surface->eglSurface = EGL_NO_SURFACE;
    }

    // Create new pbuffer at the new size
    const EGLint pbufAttribs[] = {
        EGL_WIDTH,  newW,
        EGL_HEIGHT, newH,
        EGL_NONE
    };
    surface->eglSurface = eglCreatePbufferSurface(
        surface->eglDisplay, surface->eglConfig, pbufAttribs);
    if (surface->eglSurface == EGL_NO_SURFACE) {
        PUGL_LOGE("puglX11EglResizeBuffers: eglCreatePbufferSurface failed (0x%x)",
                   eglGetError());
        return PUGL_FAILURE;
    }

    // Rebind context to new surface
    eglMakeCurrent(surface->eglDisplay, surface->eglSurface,
                   surface->eglSurface, surface->eglContext);

    // Reallocate pixel buffers
    const int stride = newW * 4;
    free(surface->readBuf);
    free(surface->xputBuf);
    surface->readBuf = (uint8_t*)calloc(1, (size_t)(stride * newH));
    surface->xputBuf = (uint8_t*)calloc(1, (size_t)(stride * newH));
    surface->bufW = newW;
    surface->bufH = newH;

    if (!surface->readBuf || !surface->xputBuf) {
        PUGL_LOGE("puglX11EglResizeBuffers: alloc failed for %dx%d", newW, newH);
        return PUGL_FAILURE;
    }

    // Recreate XImage wrapping new xputBuf
    if (surface->ximage) {
        surface->ximage->data = NULL;  // prevent XDestroyImage from freeing our buffer
        XDestroyImage(surface->ximage);
        surface->ximage = NULL;
    }
    surface->ximage = XCreateImage(
        display,
        impl->vi->visual,
        (unsigned int)impl->vi->depth,
        ZPixmap,
        0,
        (char*)surface->xputBuf,
        (unsigned int)newW,
        (unsigned int)newH,
        32,    // bitmap_pad
        stride // bytes_per_line
    );
    if (!surface->ximage) {
        PUGL_LOGE("puglX11EglResizeBuffers: XCreateImage failed");
        return PUGL_FAILURE;
    }

    PUGL_LOGD("puglX11EglResizeBuffers: success %dx%d", newW, newH);
    return PUGL_SUCCESS;
}

// ---------------------------------------------------------------------------
// configure — pick EGL config + X11 visual

static PuglStatus
puglX11EglConfigure(PuglView* view)
{
    PuglInternals* const impl = view->impl;

    PUGL_LOGD("puglX11EglConfigure: enter");

    // Use puglX11Configure to get a default XVisualInfo for window creation.
    // EGL rendering goes to an off-screen pbuffer, so the X visual only matters
    // for the window itself (XPutImage works with any visual).
    PuglStatus st = puglX11Configure(view);
    if (st != PUGL_SUCCESS) {
        PUGL_LOGE("puglX11EglConfigure: puglX11Configure failed: %d", st);
        return st;
    }

    // --- EGL init ---
    const EGLDisplay eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay == EGL_NO_DISPLAY) {
        PUGL_LOGE("puglX11EglConfigure: eglGetDisplay failed");
        return PUGL_CREATE_CONTEXT_FAILED;
    }

    int major, minor;
    if (eglInitialize(eglDisplay, &major, &minor) != EGL_TRUE) {
        PUGL_LOGE("puglX11EglConfigure: eglInitialize failed");
        return PUGL_CREATE_CONTEXT_FAILED;
    }
    PUGL_LOGD("puglX11EglConfigure: EGL %d.%d", major, minor);

    // Log EGL vendor for GPU verification
    const char* eglVendor = eglQueryString(eglDisplay, EGL_VENDOR);
    PUGL_LOGD("puglX11EglConfigure: EGL_VENDOR=%s", eglVendor ? eglVendor : "(null)");

    // Request GLES2-capable pbuffer config with fixed minimum requirements.
    // Don't pass through DPF's view hints — pbuffer configs on Android may
    // not support the depth/stencil sizes that DPF requests for window surfaces.
    // NanoVG needs stencil (for path clipping), but not depth.
    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_STENCIL_SIZE,    8,
        EGL_NONE
    };

    EGLConfig eglConfig;
    int numConfigs;
    if (eglChooseConfig(eglDisplay, configAttribs, &eglConfig, 1, &numConfigs) != EGL_TRUE
        || numConfigs < 1) {
        // Fallback: try absolute minimum (just GLES2 + pbuffer)
        PUGL_LOGD("puglX11EglConfigure: retrying with minimal config");
        const EGLint fallbackAttribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
            EGL_NONE
        };
        if (eglChooseConfig(eglDisplay, fallbackAttribs, &eglConfig, 1, &numConfigs) != EGL_TRUE
            || numConfigs < 1) {
            PUGL_LOGE("puglX11EglConfigure: eglChooseConfig failed (numConfigs=%d, err=0x%x)",
                       numConfigs, eglGetError());
            eglTerminate(eglDisplay);
            return PUGL_CREATE_CONTEXT_FAILED;
        }
    }
    PUGL_LOGD("puglX11EglConfigure: chose config, numConfigs=%d", numConfigs);

    // Allocate surface struct
    PuglX11EglSurface* const surface =
        (PuglX11EglSurface*)calloc(1, sizeof(PuglX11EglSurface));
    if (!surface) {
        eglTerminate(eglDisplay);
        return PUGL_CREATE_CONTEXT_FAILED;
    }
    impl->surface = surface;

    surface->eglDisplay = eglDisplay;
    surface->eglConfig  = eglConfig;
    surface->eglContext = EGL_NO_CONTEXT;
    surface->eglSurface = EGL_NO_SURFACE;

    // Update view hints from chosen config
    view->hints[PUGL_RED_BITS] =
        puglX11EglGetAttrib(eglDisplay, eglConfig, EGL_RED_SIZE);
    view->hints[PUGL_GREEN_BITS] =
        puglX11EglGetAttrib(eglDisplay, eglConfig, EGL_GREEN_SIZE);
    view->hints[PUGL_BLUE_BITS] =
        puglX11EglGetAttrib(eglDisplay, eglConfig, EGL_BLUE_SIZE);
    view->hints[PUGL_ALPHA_BITS] =
        puglX11EglGetAttrib(eglDisplay, eglConfig, EGL_ALPHA_SIZE);
    view->hints[PUGL_DEPTH_BITS] =
        puglX11EglGetAttrib(eglDisplay, eglConfig, EGL_DEPTH_SIZE);
    view->hints[PUGL_STENCIL_BITS] =
        puglX11EglGetAttrib(eglDisplay, eglConfig, EGL_STENCIL_SIZE);

    // pbuffer — double buffering not applicable, but DPF may check
    view->hints[PUGL_DOUBLE_BUFFER] = 0;

    PUGL_LOGD("puglX11EglConfigure: success (R=%d G=%d B=%d A=%d D=%d S=%d)",
        view->hints[PUGL_RED_BITS], view->hints[PUGL_GREEN_BITS],
        view->hints[PUGL_BLUE_BITS], view->hints[PUGL_ALPHA_BITS],
        view->hints[PUGL_DEPTH_BITS], view->hints[PUGL_STENCIL_BITS]);

    return PUGL_SUCCESS;
}

// ---------------------------------------------------------------------------
// create — EGL context + pbuffer, X11 GC + pixel buffers

static PuglStatus
puglX11EglCreate(PuglView* view)
{
    PuglInternals* const      impl    = view->impl;
    PuglX11EglSurface* const  surface = (PuglX11EglSurface*)impl->surface;
    Display* const            display = view->world->impl->display;

    if (!surface) return PUGL_FAILURE;

    // Get actual window size via XGetWindowAttributes. We can't use
    // view->lastConfigure here — it's still zero because puglRealize
    // hasn't processed any events yet. The X11 window already exists
    // (created just before this call in puglRealize), so we query it.
    XWindowAttributes attrs;
    XGetWindowAttributes(display, impl->win, &attrs);
    int w = attrs.width;
    int h = attrs.height;

    // Fallback to size hints if XGetWindowAttributes returned 0
    // (shouldn't happen, but be defensive)
    if (w <= 0 || h <= 0) {
        w = (int)view->sizeHints[PUGL_DEFAULT_SIZE].width;
        h = (int)view->sizeHints[PUGL_DEFAULT_SIZE].height;
    }

    PUGL_LOGD("puglX11EglCreate: enter, win=0x%lx size=%dx%d",
        (unsigned long)impl->win, w, h);

    // Bind GLES API before creating context
    eglBindAPI(EGL_OPENGL_ES_API);

    // Create GLES2 context
    const EGLint ctxAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    surface->eglContext = eglCreateContext(
        surface->eglDisplay, surface->eglConfig, EGL_NO_CONTEXT, ctxAttribs);
    if (surface->eglContext == EGL_NO_CONTEXT) {
        PUGL_LOGE("puglX11EglCreate: eglCreateContext failed (0x%x)", eglGetError());
        return PUGL_CREATE_CONTEXT_FAILED;
    }
    PUGL_LOGD("puglX11EglCreate: EGL context created");

    // Create pbuffer surface
    const EGLint pbufAttribs[] = {
        EGL_WIDTH,  w > 0 ? w : 1,
        EGL_HEIGHT, h > 0 ? h : 1,
        EGL_NONE
    };
    surface->eglSurface = eglCreatePbufferSurface(
        surface->eglDisplay, surface->eglConfig, pbufAttribs);
    if (surface->eglSurface == EGL_NO_SURFACE) {
        PUGL_LOGE("puglX11EglCreate: eglCreatePbufferSurface failed (0x%x)", eglGetError());
        eglDestroyContext(surface->eglDisplay, surface->eglContext);
        surface->eglContext = EGL_NO_CONTEXT;
        return PUGL_CREATE_CONTEXT_FAILED;
    }
    PUGL_LOGD("puglX11EglCreate: pbuffer %dx%d created", w, h);

    // Make context current temporarily to query GL renderer
    eglMakeCurrent(surface->eglDisplay, surface->eglSurface,
                   surface->eglSurface, surface->eglContext);
    {
        const char* glVendor   = (const char*)glGetString(GL_VENDOR);
        const char* glRenderer = (const char*)glGetString(GL_RENDERER);
        const char* glVersion  = (const char*)glGetString(GL_VERSION);
        PUGL_LOGD("puglX11EglCreate: GL_VENDOR   = %s", glVendor   ? glVendor   : "(null)");
        PUGL_LOGD("puglX11EglCreate: GL_RENDERER = %s", glRenderer ? glRenderer : "(null)");
        PUGL_LOGD("puglX11EglCreate: GL_VERSION  = %s", glVersion  ? glVersion  : "(null)");
    }
    eglMakeCurrent(surface->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    // X11 Graphics Context for XPutImage
    surface->gc = XCreateGC(display, impl->win, 0, NULL);

    // Allocate pixel buffers
    if (w > 0 && h > 0) {
        const int stride = w * 4;
        surface->bufW    = w;
        surface->bufH    = h;
        surface->readBuf = (uint8_t*)calloc(1, (size_t)(stride * h));
        surface->xputBuf = (uint8_t*)calloc(1, (size_t)(stride * h));

        if (!surface->readBuf || !surface->xputBuf) {
            PUGL_LOGE("puglX11EglCreate: pixel buffer alloc failed");
            return PUGL_CREATE_CONTEXT_FAILED;
        }

        // Create XImage wrapping xputBuf (BGRA / ZPixmap / 32bpp)
        surface->ximage = XCreateImage(
            display,
            impl->vi->visual,
            (unsigned int)impl->vi->depth,
            ZPixmap,
            0,
            (char*)surface->xputBuf,
            (unsigned int)w,
            (unsigned int)h,
            32,    // bitmap_pad
            stride // bytes_per_line
        );
        if (!surface->ximage) {
            PUGL_LOGE("puglX11EglCreate: XCreateImage failed");
            return PUGL_CREATE_CONTEXT_FAILED;
        }
    } else {
        // Zero-size window — defer buffer creation to first resize
        PUGL_LOGD("puglX11EglCreate: zero-size window, deferring buffer creation");
        surface->bufW = 0;
        surface->bufH = 0;
    }

    PUGL_LOGD("puglX11EglCreate: success");
    return PUGL_SUCCESS;
}

// ---------------------------------------------------------------------------
// destroy

static void
puglX11EglDestroy(PuglView* view)
{
    PuglX11EglSurface* surface = (PuglX11EglSurface*)view->impl->surface;
    if (!surface) return;

    Display* const display = view->world->impl->display;

    PUGL_LOGD("puglX11EglDestroy: enter");

    // Unbind context
    eglMakeCurrent(surface->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (surface->eglSurface != EGL_NO_SURFACE) {
        eglDestroySurface(surface->eglDisplay, surface->eglSurface);
    }
    if (surface->eglContext != EGL_NO_CONTEXT) {
        eglDestroyContext(surface->eglDisplay, surface->eglContext);
    }
    eglTerminate(surface->eglDisplay);

    if (surface->ximage) {
        // Prevent XDestroyImage from freeing our managed buffer
        surface->ximage->data = NULL;
        XDestroyImage(surface->ximage);
    }
    if (surface->gc) {
        XFreeGC(display, surface->gc);
    }

    free(surface->readBuf);
    free(surface->xputBuf);
    free(surface);
    view->impl->surface = NULL;

    PUGL_LOGD("puglX11EglDestroy: done");
}

// ---------------------------------------------------------------------------
// enter — make EGL context current, handle resize

PUGL_WARN_UNUSED_RESULT
static PuglStatus
puglX11EglEnter(PuglView* view, const PuglExposeEvent* PUGL_UNUSED(expose))
{
    PuglX11EglSurface* const surface = (PuglX11EglSurface*)view->impl->surface;
    if (!surface || surface->eglContext == EGL_NO_CONTEXT) {
        return PUGL_FAILURE;
    }

    // Check for resize: view->lastConfigure may have been updated by a
    // previous event cycle's puglConfigure(). If so, resize our EGL
    // pbuffer and pixel buffers to match.
    int newW = (int)view->lastConfigure.width;
    int newH = (int)view->lastConfigure.height;

    if (newW > 0 && newH > 0 &&
        (newW != surface->bufW || newH != surface->bufH)) {
        // puglX11EglResizeBuffers handles unbind/rebind of EGL context
        PuglStatus st = puglX11EglResizeBuffers(view, newW, newH);
        if (st != PUGL_SUCCESS) return st;
        // Context is already current after resize
        return PUGL_SUCCESS;
    }

    if (eglMakeCurrent(surface->eglDisplay, surface->eglSurface,
                       surface->eglSurface, surface->eglContext) != EGL_TRUE) {
        PUGL_LOGE("puglX11EglEnter: eglMakeCurrent failed (0x%x)", eglGetError());
        return PUGL_FAILURE;
    }

    return PUGL_SUCCESS;
}

// ---------------------------------------------------------------------------
// leave — readback pixels and blit to X11

PUGL_WARN_UNUSED_RESULT
static PuglStatus
puglX11EglLeave(PuglView* view, const PuglExposeEvent* expose)
{
    PuglX11EglSurface* const surface = (PuglX11EglSurface*)view->impl->surface;
    PuglInternals* const     impl    = view->impl;
    Display* const           display = view->world->impl->display;

    if (expose) {
        // Check if puglConfigure (which runs between enter and leave)
        // changed the view size. If so, resize buffers now and skip this
        // frame's blit — the content was rendered to the wrong-size pbuffer.
        int newW = (int)view->lastConfigure.width;
        int newH = (int)view->lastConfigure.height;
        if (newW > 0 && newH > 0 &&
            (newW != surface->bufW || newH != surface->bufH)) {
            PUGL_LOGD("puglX11EglLeave: resize detected (%dx%d -> %dx%d), resizing + skip blit",
                surface->bufW, surface->bufH, newW, newH);
            puglX11EglResizeBuffers(view, newW, newH);
            // Skip blit — next frame will draw to correctly-sized buffers
            goto done;
        }

        const int w = surface->bufW;
        const int h = surface->bufH;

        if (w > 0 && h > 0 && surface->readBuf && surface->ximage) {
            // 1. GPU -> CPU: read back the rendered frame
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, surface->readBuf);

            // 2. RGBA->BGRA swizzle + vertical flip (single pass)
            const int stride = w * 4;
            for (int y = 0; y < h; y++) {
                const uint32_t* src = (const uint32_t*)(surface->readBuf + (h - 1 - y) * stride);
                uint32_t*       dst = (uint32_t*)(surface->xputBuf + y * stride);
                for (int x = 0; x < w; x++) {
                    const uint32_t p = src[x]; // 0xAABBGGRR on little-endian
                    // Swap R and B: keep G and A in place
                    dst[x] = (p & 0xFF00FF00u)
                           | ((p >> 16) & 0xFFu)
                           | ((p & 0xFFu) << 16);
                }
            }

            // 3. CPU -> X11 server: blit via XPutImage
            XPutImage(display, impl->win, surface->gc, surface->ximage,
                      0, 0, 0, 0, (unsigned int)w, (unsigned int)h);
            XFlush(display);
        }
    }

done:
    eglMakeCurrent(surface->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return PUGL_SUCCESS;
}

// ---------------------------------------------------------------------------
// Public API

PuglGlFunc
puglGetProcAddress(const char* name)
{
    return (PuglGlFunc)eglGetProcAddress(name);
}

PuglStatus
puglEnterContext(PuglView* view)
{
    return view->backend->enter(view, NULL);
}

PuglStatus
puglLeaveContext(PuglView* view)
{
    return view->backend->leave(view, NULL);
}

const PuglBackend*
puglGlBackend(void)
{
    static const PuglBackend backend = {
        puglX11EglConfigure,
        puglX11EglCreate,
        puglX11EglDestroy,
        puglX11EglEnter,
        puglX11EglLeave,
        puglStubGetContext
    };
    return &backend;
}
