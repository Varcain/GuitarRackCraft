// AhbTexture.cpp — see AhbTexture.h. Consumer half of the Phase 0 spike,
// productionised: import an AHardwareBuffer as a GLES texture via
// EGL_ANDROID_image_native_buffer.

#include "AhbTexture.h"

#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <cstring>
#include <dlfcn.h>

#define TAG "X11Ahb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace guitarrackcraft {

namespace {
bool hasEglExt(const char* exts, const char* want) {
    if (!exts || !want) return false;
    size_t n = strlen(want);
    const char* p = exts;
    while ((p = strstr(p, want))) {
        char before = (p == exts) ? ' ' : p[-1];
        char after  = p[n];
        if ((before == ' ' || before == '\0') && (after == ' ' || after == '\0')) return true;
        p += n;
    }
    return false;
}
} // namespace

bool AhbImporter::init(EGLDisplay dpy) {
    if (inited_) return supported_;
    inited_ = true;
    dpy_ = dpy;

    const char* eglExts = eglQueryString(dpy, EGL_EXTENSIONS);
    const char* glExts  = (const char*)glGetString(GL_EXTENSIONS);
    bool extOk = hasEglExt(eglExts, "EGL_ANDROID_image_native_buffer") &&
                 hasEglExt(eglExts, "EGL_KHR_image_base") &&
                 hasEglExt(glExts,  "GL_OES_EGL_image");

    pGetNativeClientBuffer_ = (void*)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    pCreateImage_           = (void*)eglGetProcAddress("eglCreateImageKHR");
    pDestroyImage_          = (void*)eglGetProcAddress("eglDestroyImageKHR");
    pImageTargetTexture2D_  = (void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    supported_ = extOk && pGetNativeClientBuffer_ && pCreateImage_ && pImageTargetTexture2D_;

    // Native-fence sync (cross-driver producer→consumer wait). Optional: absent
    // support just means we skip the GPU wait (close the fd) — acceptable for a
    // static editor, glitchy under fast animation.
    bool fenceExt = hasEglExt(eglExts, "EGL_ANDROID_native_fence_sync") &&
                    hasEglExt(eglExts, "EGL_KHR_fence_sync");
    pCreateSync_  = (void*)eglGetProcAddress("eglCreateSyncKHR");
    pDestroySync_ = (void*)eglGetProcAddress("eglDestroySyncKHR");
    pWaitSync_    = (void*)eglGetProcAddress("eglWaitSyncKHR");
    fenceSupported_ = fenceExt && pCreateSync_ && pDestroySync_ && pWaitSync_;

    // AHardwareBuffer_getId (API 31+) gives a stable id per gralloc buffer, so we
    // can cache EGLImages across the producer's cycling pool. Resolve from the
    // already-loaded libandroid (RTLD_DEFAULT); null on API<31 → ephemeral path.
    pGetId_ = dlsym(RTLD_DEFAULT, "AHardwareBuffer_getId");

    LOGI("AhbImporter::init supported=%d fence=%d imgCache=%d (ext=%d getNCB=%p createImg=%p target2D=%p)",
         supported_, fenceSupported_, pGetId_ != nullptr, extOk,
         pGetNativeClientBuffer_, pCreateImage_, pImageTargetTexture2D_);
    return supported_;
}

void AhbImporter::onContextLost() {
    auto destroyImg = (PFNEGLDESTROYIMAGEKHRPROC)pDestroyImage_;
    if (destroyImg && dpy_ != EGL_NO_DISPLAY) {
        for (int i = 0; i < imgCacheCount_; ++i)
            if (imgCache_[i].image) destroyImg(dpy_, (EGLImageKHR)imgCache_[i].image);
        if (image_ != EGL_NO_IMAGE_KHR) destroyImg(dpy_, (EGLImageKHR)image_);
    }
    imgCacheCount_ = 0;
    image_ = nullptr;
    tex_ = 0;  // texture died with the old context
}

bool AhbImporter::waitFenceFd(int fenceFd) {
    if (fenceFd < 0) return false;
    if (!fenceSupported_) return false;
    auto createSync  = (PFNEGLCREATESYNCKHRPROC)pCreateSync_;
    auto destroySync = (PFNEGLDESTROYSYNCKHRPROC)pDestroySync_;
    auto waitSync    = (PFNEGLWAITSYNCKHRPROC)pWaitSync_;
    // EGL_SYNC_NATIVE_FENCE_ANDROID takes ownership of the fd on success.
    const EGLint attrs[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fenceFd, EGL_NONE };
    EGLSyncKHR sync = createSync(dpy_, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
    if (sync == EGL_NO_SYNC_KHR) {
        LOGE("waitFenceFd: eglCreateSyncKHR err=0x%x (fd not consumed)", eglGetError());
        return false;  // caller still owns fenceFd
    }
    // Server-side GPU wait: the compositor's draw queues behind the producer's
    // write without stalling the X-server CPU thread.
    waitSync(dpy_, sync, 0);
    destroySync(dpy_, sync);
    return true;  // fd consumed by the EGLSync
}

GLuint AhbImporter::importToTexture(AHardwareBuffer* ahb) {
    if (!supported_ || !ahb) return 0;

    auto getNCB = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)pGetNativeClientBuffer_;
    auto createImg = (PFNEGLCREATEIMAGEKHRPROC)pCreateImage_;
    auto destroyImg = (PFNEGLDESTROYIMAGEKHRPROC)pDestroyImage_;
    auto targetTex2D = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)pImageTargetTexture2D_;

    // Stable per-buffer id (API 31+) → cache the EGLImage across the producer's
    // cycling pool, so we create it once per buffer and only re-bind thereafter.
    unsigned long long id = 0;
    bool haveId = false;
    if (pGetId_) {
        auto getId = (int (*)(const AHardwareBuffer*, uint64_t*))pGetId_;
        uint64_t v = 0;
        if (getId(ahb, &v) == 0) { id = v; haveId = true; }
    }

    EGLImageKHR img = EGL_NO_IMAGE_KHR;
    if (haveId) {
        for (int i = 0; i < imgCacheCount_; ++i)
            if (imgCache_[i].id == id) { img = (EGLImageKHR)imgCache_[i].image; break; }
    }

    if (img == EGL_NO_IMAGE_KHR) {
        EGLClientBuffer clientBuf = getNCB(ahb);
        if (!clientBuf) { LOGE("importToTexture: eglGetNativeClientBufferANDROID NULL"); return 0; }
        const EGLint imgAttr[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
        img = createImg(dpy_, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuf, imgAttr);
        if (img == EGL_NO_IMAGE_KHR) { LOGE("importToTexture: eglCreateImageKHR err=0x%x", eglGetError()); return 0; }

        if (haveId) {
            // Cache it. Evict the oldest (FIFO) if full — the pool is small so
            // this rarely fires once steady-state.
            if (imgCacheCount_ < kImgCache) {
                imgCache_[imgCacheCount_++] = { id, img };
            } else {
                if (imgCache_[0].image && destroyImg) destroyImg(dpy_, (EGLImageKHR)imgCache_[0].image);
                for (int i = 1; i < kImgCache; ++i) imgCache_[i - 1] = imgCache_[i];
                imgCache_[kImgCache - 1] = { id, img };
            }
        } else {
            // No stable id: ephemeral single image, recreate each call (old path).
            if (image_ != EGL_NO_IMAGE_KHR && destroyImg) destroyImg(dpy_, (EGLImageKHR)image_);
            image_ = img;
        }
    }

    if (tex_ == 0) glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    targetTex2D(GL_TEXTURE_2D, (GLeglImageOES)img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) { LOGE("importToTexture: glEGLImageTargetTexture2DOES glError=0x%x", e); return 0; }
    return tex_;
}

void AhbImporter::destroy() {
    auto destroyImg = (PFNEGLDESTROYIMAGEKHRPROC)pDestroyImage_;
    if (destroyImg) {
        if (image_ != EGL_NO_IMAGE_KHR) destroyImg(dpy_, (EGLImageKHR)image_);
        for (int i = 0; i < imgCacheCount_; ++i)
            if (imgCache_[i].image) destroyImg(dpy_, (EGLImageKHR)imgCache_[i].image);
    }
    image_ = EGL_NO_IMAGE_KHR;
    imgCacheCount_ = 0;
    if (tex_) { glDeleteTextures(1, &tex_); tex_ = 0; }
}

AhbImporter::~AhbImporter() {
    // Note: a correct teardown needs a current context; the render thread leaks
    // its GL objects on exit anyway (see X11NativeDisplay renderLoop teardown),
    // so don't touch GL here from an arbitrary thread.
}

} // namespace guitarrackcraft
