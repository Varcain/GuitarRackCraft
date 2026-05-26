// JNI surface for the in-process X11 server.
//
// The server itself lives in app/src/main/cpp/x11/ (ported from
// GuitarRackCraft). It listens on TCP 127.0.0.1:6000+N per display number
// and renders X11 client output to an Android Surface via EGL/GLES2.
//
// These bindings only expose what we currently need from Kotlin —
// attach/detach a Surface, inject touches, resize. We can grow the surface
// as the integration with vst_host + wine matures.

#include "util/log.h"
#include "x11/DisplayState.h"
#include "x11/X11NativeDisplay.h"

#include <jni.h>

using guitarrackcraft::displayStateMutex;
using guitarrackcraft::displayStates;
using guitarrackcraft::DisplayState;
using guitarrackcraft::destroyX11Display;
using guitarrackcraft::getX11Display;
using guitarrackcraft::getOrCreateX11Display;
using guitarrackcraft::withDisplayInjectTouch;
using guitarrackcraft::withDisplayInjectKey;
using guitarrackcraft::withDisplayRequestFrame;
using guitarrackcraft::withDisplaySetSurfaceSize;
using guitarrackcraft::withDisplaySetPluginSize;
using guitarrackcraft::withDisplaySetFramebufferFrozen;
using guitarrackcraft::withDisplayStartServer;
using guitarrackcraft::withDisplayGetRenderedExtent;
using guitarrackcraft::X11NativeDisplay;

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_varcain_vsthost_NativeBridge_nativeAttachSurfaceToX11Display(
    JNIEnv* env, jobject /*thiz*/,
    jint displayNumber, jobject surface, jint width, jint height) {
    if (!surface || width <= 0 || height <= 0) {
        LOGE("attachSurfaceToX11Display: invalid args display=%d w=%d h=%d",
             (int)displayNumber, (int)width, (int)height);
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(displayStateMutex());
        auto& s = displayStates()[displayNumber];
        s.phase = DisplayState::Phase::Attached;
        s.pluginIndex = -1;
        s.detachPending = false;
    }
    X11NativeDisplay* disp = getOrCreateX11Display(displayNumber);
    if (!disp->attachSurface(env, surface, width, height)) {
        LOGE("attachSurfaceToX11Display: attach failed for display=%d",
             (int)displayNumber);
        std::lock_guard<std::mutex> lock(displayStateMutex());
        displayStates().erase(displayNumber);
        return 0;
    }
    const jlong rootId = static_cast<jlong>(disp->getRootWindowId());
    LOGI("X11 display %d attached, root=%lld", (int)displayNumber,
         (long long)rootId);
    return rootId;
}

JNIEXPORT void JNICALL
Java_com_varcain_vsthost_NativeBridge_nativeDetachAndDestroyX11Display(
    JNIEnv* /*env*/, jobject /*thiz*/, jint displayNumber) {
    X11NativeDisplay* disp = getX11Display(displayNumber);
    if (disp) {
        disp->detachSurface();
        destroyX11Display(displayNumber);
    }
    std::lock_guard<std::mutex> lock(displayStateMutex());
    displayStates().erase(displayNumber);
    LOGI("X11 display %d destroyed", (int)displayNumber);
}

JNIEXPORT void JNICALL
Java_com_varcain_vsthost_NativeBridge_nativeSetX11SurfaceSize(
    JNIEnv* /*env*/, jobject /*thiz*/,
    jint displayNumber, jint width, jint height) {
    withDisplaySetSurfaceSize(displayNumber, width, height);
}

JNIEXPORT void JNICALL
Java_com_varcain_vsthost_NativeBridge_nativeSetX11PluginSize(
    JNIEnv* /*env*/, jobject /*thiz*/,
    jint displayNumber, jint width, jint height) {
    withDisplaySetPluginSize(displayNumber, width, height);
}

JNIEXPORT void JNICALL
Java_com_varcain_vsthost_NativeBridge_nativeStartX11Server(
    JNIEnv* /*env*/, jobject /*thiz*/,
    jint displayNumber, jint placeholderW, jint placeholderH) {
    withDisplayStartServer(displayNumber, placeholderW, placeholderH);
}

JNIEXPORT void JNICALL
Java_com_varcain_vsthost_NativeBridge_nativeInjectX11Touch(
    JNIEnv* /*env*/, jobject /*thiz*/,
    jint displayNumber, jint action, jint x, jint y) {
    withDisplayInjectTouch(displayNumber, action, x, y);
}

JNIEXPORT void JNICALL
Java_com_varcain_vsthost_NativeBridge_nativeInjectX11Key(
    JNIEnv* /*env*/, jobject /*thiz*/,
    jint displayNumber, jint action, jint keycode, jint state) {
    withDisplayInjectKey(displayNumber, action, keycode, state);
}

JNIEXPORT void JNICALL
Java_com_varcain_vsthost_NativeBridge_nativeRequestX11Frame(
    JNIEnv* /*env*/, jobject /*thiz*/, jint displayNumber) {
    withDisplayRequestFrame(displayNumber);
}

/* Fills result[0]=w, result[1]=h with the bounding box of pixels actually
 * written into the active plugin slot. Both 0 if nothing has been rendered
 * yet. Used by HostViewModel to detect the real editor extent for plugins
 * (like AmpCraft) whose effEditGetRect lies. */
JNIEXPORT void JNICALL
Java_com_varcain_vsthost_NativeBridge_nativeGetX11RenderedExtent(
    JNIEnv* env, jobject /*thiz*/, jint displayNumber, jintArray result) {
    if (!result) return;
    if (env->GetArrayLength(result) < 2) return;
    int w = 0, h = 0;
    withDisplayGetRenderedExtent(displayNumber, w, h);
    jint vals[2] = { static_cast<jint>(w), static_cast<jint>(h) };
    env->SetIntArrayRegion(result, 0, 2, vals);
}

/* Freeze (or unfreeze) the framebuffer size for a display. While
 * frozen, automatic slot-promotion paths in the X server no longer
 * resize the framebuffer to match a window's bounds — they just track
 * the slot for touch routing. Used by the installer flow so a small
 * wizard window doesn't shrink the desktop framebuffer it then paints
 * into at higher coords. */
JNIEXPORT void JNICALL
Java_com_varcain_vsthost_NativeBridge_nativeSetX11FramebufferFrozen(
    JNIEnv* /*env*/, jobject /*thiz*/, jint displayNumber, jboolean frozen) {
    withDisplaySetFramebufferFrozen(displayNumber, frozen != JNI_FALSE);
}

}  // extern "C"
