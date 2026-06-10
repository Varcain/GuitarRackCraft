// AhbTexture.h — Phase 1 of the GPU X-server upgrade.
//
// Imports an AHardwareBuffer (rendered by wine/Turnip, or CPU-filled for the
// synthetic test) as a GLES texture via EGL_ANDROID_image_native_buffer, so the
// compositor can sample it for a window's quad instead of uploading the CPU
// framebuffer — the zero-copy editor present. The import/sample primitive is the
// consumer half of the Phase 0 spike, proven on-device (Adreno 750 + Turnip).
//
// All methods touch GL/EGL state and MUST be called on the render thread (the
// one holding the EGL context), EXCEPT the trivial accessors.
#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>

struct AHardwareBuffer;

namespace guitarrackcraft {

class AhbImporter {
public:
    // Resolve EGL/GLES AHB-import entrypoints + probe extension support. Call
    // once with a current GL context. Idempotent. Returns supported().
    bool init(EGLDisplay dpy);
    bool supported() const { return supported_; }

    // Import `ahb` as a GL_TEXTURE_2D, returning the texture name (owned by this
    // importer — replaced on the next call, freed by destroy()/dtor). Does NOT
    // take a reference on the AHB; the caller owns the AHB's lifetime and must
    // keep it alive while the returned texture is in use. Returns 0 on failure.
    GLuint importToTexture(AHardwareBuffer* ahb);

    // GPU-wait a producer's sync-fd (cross-driver: Turnip write → Adreno read)
    // before the imported texture is sampled. Wraps the fd as an
    // EGL_ANDROID_native_fence_sync and eglWaitSyncKHR's it (server-side GPU
    // wait, no CPU stall). CONSUMES `fenceFd` on success (the EGLSync takes
    // ownership). Returns false (and leaves the fd to the caller) if unsupported
    // or creation fails. Needs a current GL context.
    bool waitFenceFd(int fenceFd);

    // Free the current EGLImage + texture. Needs a current GL context.
    void destroy();

    // The GL context was lost/recreated (re-attach): forget the texture and drop
    // the EGLImage cache (its backing buffers belong to the now-gone producer
    // connection). Safe to call from initGL (a live EGLDisplay); destroys cached
    // EGLImages. The next importToTexture() reallocates.
    void onContextLost();

    ~AhbImporter();

private:
    EGLDisplay dpy_ = EGL_NO_DISPLAY;
    bool inited_ = false;
    bool supported_ = false;
    void* image_ = nullptr;   // EGLImageKHR (void*) — kept opaque to avoid eglext in the header
    GLuint tex_ = 0;

    // Resolved entrypoints (typed void* to avoid leaking eglext into the header).
    void* pGetNativeClientBuffer_ = nullptr;
    void* pCreateImage_ = nullptr;
    void* pDestroyImage_ = nullptr;
    void* pImageTargetTexture2D_ = nullptr;
    // Native-fence sync entrypoints (EGL_ANDROID_native_fence_sync). May be null
    // if unsupported → waitFenceFd() returns false and the caller closes the fd.
    void* pCreateSync_ = nullptr;
    void* pDestroySync_ = nullptr;
    void* pWaitSync_ = nullptr;
    bool  fenceSupported_ = false;

    // EGLImage cache keyed by AHardwareBuffer stable id (AHardwareBuffer_getId,
    // API 31+). A streaming producer cycles a small fixed buffer pool; without
    // this we'd eglCreateImageKHR+eglDestroyImageKHR EVERY frame (expensive). With
    // it, each pool buffer's EGLImage is created once and re-bound thereafter.
    // pGetId_ null (API<31 / unavailable) → fall back to the ephemeral single
    // image_ path (recreate per frame).
    void* pGetId_ = nullptr;
    static constexpr int kImgCache = 8;
    struct CacheEntry { unsigned long long id; void* image; };  // image = EGLImageKHR
    CacheEntry imgCache_[kImgCache] = {};
    int imgCacheCount_ = 0;
};

} // namespace guitarrackcraft
