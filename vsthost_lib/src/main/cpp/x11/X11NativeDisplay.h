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

#ifndef GUITARRACKCRAFT_X11_NATIVE_DISPLAY_H
#define GUITARRACKCRAFT_X11_NATIVE_DISPLAY_H

#include <android/native_window.h>
#include <jni.h>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace guitarrackcraft {

/**
 * Manages a single X11 display: ANativeWindow binding, EGL surface,
 * and a minimal X11 server that renders to the surface (zero-copy path).
 * One instance per display number (e.g. :10 -> port 6010).
 */
class X11NativeDisplay {
public:
    explicit X11NativeDisplay(int displayNumber);
    ~X11NativeDisplay();

    /** Attach an Android Surface (creates ANativeWindow, EGL surface, starts X server). */
    bool attachSurface(JNIEnv* env, jobject surface, int width, int height);

    /** Detach surface and stop X server. */
    void detachSurface();

    /** Signal threads to exit without joining (call from view destroy callback to avoid blocking).
     * Returns true if the detach was deferred (e.g., plugin creation in progress), false if executed immediately. */
    bool signalDetach();

    /** Stop only the render thread and wait for it to exit. Use when surface is destroyed but we must
     * not close the X fd yet (plugin may still be in XGetWindowAttributes). Avoids eglSwapBuffers
     * on a destroyed surface and avoids EGL teardown that can crash HWUI.
     * This method blocks until the render thread has fully exited. */
    void stopRenderThreadOnly();

    /** Start/restart the render thread. Call this after stopRenderThreadOnly() to resume rendering.
     * Used when switching back to X11 UI mode after hiding the display. */
    void startRenderThread();

    /** Update surface size (e.g. on surfaceChanged). */
    void setSurfaceSize(int width, int height);

    /** Root window ID for XCreateWindow parent (e.g. plugin UI). */
    unsigned long getRootWindowId() const { return rootWindowId_; }

    /** Inject pointer event: action (0=down, 1=up, 2=move, 3=right-click tap),
     *  x, y in view coordinates. action=3 is an atomic ButtonPress(3) +
     *  ButtonRelease(3) at (x,y) used for two-finger-tap → right-click. */
    void injectTouch(int action, int x, int y);

    /** Inject a key press/release into the focused editor's window.
     *  action 0=press, 1=release; keycode is the X11 hardware keycode in
     *  our kJavaKeymapPayload table; state is the X11 modifier mask
     *  (Shift=0x01, Control=0x04). Queued for the connection thread to
     *  send so we don't race xcb. */
    void injectKey(int action, int keycode, int state);

    /** Hit-test: returns true if (surfaceX, surfaceY) hits an X11 widget (knob, slider, etc.)
     *  rather than the plugin background. Thread-safe (called from Android UI thread). */
    bool isWidgetAtPoint(int surfaceX, int surfaceY);

    /** Request a frame (swap buffers); call from vsync or idle. */
    void requestFrame();

    /** Set an idle callback to be called from the plugin UI thread.
     * This ensures all X11 operations (idle, instantiate, cleanup) happen
     * on a single dedicated thread, preventing xcb_xlib_threads_sequence_lost. */
    using IdleCallback = std::function<void()>;
    void setIdleCallback(IdleCallback callback);

    /** Post a task to the plugin UI thread (non-blocking). */
    void postTask(std::function<void()> task);

    /** Post a task to the plugin UI thread and wait for completion (blocking). */
    void postTaskAndWait(std::function<void()> task);

    bool isAttached() const;
    /** Returns true if server is using Unix domain socket instead of TCP */
    bool isUnixSocket() const;

    /** Get the plugin's natural window size (from first CreateWindow call). Returns false if not yet known. */
    bool getPluginSize(int& w, int& h);

    /** Bounding box of pixels actually written by the active plugin into the
     *  framebuffer. Differs from getPluginSize when the plugin's declared
     *  CreateWindow size doesn't match what its renderer paints (AmpCraft
     *  declares 1290x612 but DXVK presents 896x612). Returns (0,0) until the
     *  first PutImage lands. Compose polls this to size the SurfaceView to
     *  the rendered area, not the declared area. */
    void getRenderedExtent(int& w, int& h);

    /** Override the plugin's natural size + resize the framebuffer. Used
     *  when the auto-detect from CreateWindow misses (JUCE editors that
     *  attach as children of small parents). */
    void setPluginSize(int w, int h);

    /** Read-only snapshot of the plugin's rendered framebuffer. Copies the
     *  current pixels (32-bit, X11 wire order — treat as ARGB8888) into out
     *  and sets w/h to the framebuffer dimensions. Thread-safe (locks the
     *  bufferMutex). Returns false if the framebuffer is empty. Used by the
     *  desktop test harness to mirror the plugin UI into an SDL window
     *  without going through the EGL/GLES render path. */
    bool snapshotFramebuffer(std::vector<uint32_t>& out, int& w, int& h);

    /** Lock/unlock the framebuffer size against auto slot-promotion.
     *  When frozen, CreateWindow/PutImage that would otherwise shrink
     *  the framebuffer to the new slot's bounds just record the slot
     *  for touch routing and leave the framebuffer alone. Used by the
     *  installer flow so a small wizard window doesn't shrink the
     *  framebuffer below the desktop coordinate space it then paints
     *  into (the wizard moves itself to e.g. (507, 445) inside a 1024x768
     *  desktop). */
    void setFramebufferFrozen(bool frozen);

    /** Start the X11 protocol server without an attached Surface. wine
     *  clients can connect immediately; on-screen rendering only happens
     *  once attachSurface() is also called. */
    bool startServer(int placeholderW, int placeholderH);

    /** TCP port the X11 server actually bound on. Normally
     *  kX11BasePort + displayNumber (6001 for display 1) but if that port
     *  was held by an orphan, serverLoop steps up to 6101, 6201, ….
     *  Returns -1 before startServer or if all attempts failed. Callers
     *  set DISPLAY=127.0.0.1:(port-6000) so wine connects to the new
     *  listener instead of the orphan's accept backlog. */
    int getActualPort() const;

    /** Set UI scale factor (< 1.0 = smaller plugin rendering = faster). Must be called before plugin connects. */
    void setUIScale(float scale);

    /** Get the current UI scale factor. */
    float getUIScale() const;

    /** Phase 1 GPU-present validation hook. on=true: allocate an
     *  AHardwareBuffer at the editor size, CPU-fill a gradient, and register
     *  it as the editor's GPU source — the compositor samples it via EGLImage
     *  instead of uploading the CPU framebuffer (zero-copy path). on=false:
     *  clear it, reverting to the CPU framebuffer. Returns false if the AHB
     *  allocation/lock fails. Synthetic; needs no plugin/wine. */
    bool debugSetEditorAhbGradient(bool on);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int displayNumber_;
    unsigned long rootWindowId_ = 1;  // Fixed root id for minimal server
    std::mutex mutex_;
};

/** Global access: get or create display for display number. Used by JNI. */
X11NativeDisplay* getOrCreateX11Display(int displayNumber);
/** Get existing display or null if not found (does not create). */
X11NativeDisplay* getX11Display(int displayNumber);
void destroyX11Display(int displayNumber);

/** Call display methods while holding the display map lock (avoids TOCTOU use-after-free). */
void withDisplayRequestFrame(int displayNumber);
void withDisplayInjectTouch(int displayNumber, int action, int x, int y);
void withDisplayInjectKey(int displayNumber, int action, int keycode, int state);
void withDisplaySetSurfaceSize(int displayNumber, int width, int height);
void withDisplaySetPluginSize(int displayNumber, int w, int h);
/** Freeze the framebuffer size on this display against auto slot-promotion.
 *  Installer flow sets this to true while the wine wizard runs. */
void withDisplaySetFramebufferFrozen(int displayNumber, bool frozen);
void withDisplayStartServer(int displayNumber, int placeholderW, int placeholderH);
/** Get the actual TCP port the display's X11 server bound on. -1 if not
 *  started. WineVstPlugin reads this to compute the right DISPLAY env. */
int withDisplayGetActualPort(int displayNumber);
/** Returns true and fills w/h if plugin natural size is known; false if not yet set. */
bool withDisplayGetPluginSize(int displayNumber, int& w, int& h);
/** Returns true and fills w/h with the actually-rendered extent (0 each if no
 *  PutImage has landed yet). See X11NativeDisplay::getRenderedExtent. */
bool withDisplayGetRenderedExtent(int displayNumber, int& w, int& h);
/** Snapshot the display's framebuffer while holding the display map lock
 *  (avoids TOCTOU use-after-free). See X11NativeDisplay::snapshotFramebuffer. */
bool withDisplaySnapshotFramebuffer(int displayNumber, std::vector<uint32_t>& out,
                                    int& w, int& h);
/** Returns the UI scale factor for the given display (1.0 = no scaling). */
float withDisplayGetUIScale(int displayNumber);
/** Hit-test: returns true if (x, y) hits an X11 widget rather than plugin background. */
bool withDisplayIsWidgetAtPoint(int displayNumber, int x, int y);
/** Phase 1 synthetic GPU-present hook: register (on=true) or clear (on=false)
 *  a CPU-filled gradient AHardwareBuffer as the editor's GPU source on this
 *  display. See X11NativeDisplay::debugSetEditorAhbGradient. */
bool withDisplayDebugSetEditorAhbGradient(int displayNumber, bool on);
/** Post a task to the display's plugin UI thread while holding the display map lock (avoids TOCTOU). */
void withDisplayPostTask(int displayNumber, std::function<void()> task);
/** Set the idle callback while holding the display map lock (avoids TOCTOU). */
void withDisplaySetIdleCallback(int displayNumber, X11NativeDisplay::IdleCallback cb);
/** Post a task and wait for completion while holding the display map lock during post.
 *  Returns true if the display was found and the task was posted, false if display gone. */
bool withDisplayPostTaskAndWait(int displayNumber, std::function<void()> task);

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_X11_NATIVE_DISPLAY_H
