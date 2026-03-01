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

@file:OptIn(androidx.compose.ui.ExperimentalComposeUiApi::class)

package com.varcain.guitarrackcraft.ui.x11

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.Choreographer
import android.view.MotionEvent
import android.graphics.SurfaceTexture
import android.view.Surface
import android.view.SurfaceView
import android.view.TextureView
import android.view.View
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.ui.input.pointer.pointerInteropFilter
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.key
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.viewinterop.AndroidView
import com.varcain.guitarrackcraft.engine.X11Bridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.delay
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlin.coroutines.resume

private const val TAG = "PluginX11UiView"
private const val VELOCITY_SCALE = 0.35f
private const val X11_INIT_DELAY_MS = 400L
private const val X11_CLEANUP_DELAY_MS = 2000L
/** Use TextureView instead of SurfaceView (TextureView uses SurfaceTexture).
 * TextureView is detached from the window hierarchy and shouldn't affect HWUI threads.
 * This avoids the "pthread_mutex_lock on destroyed mutex" crash in HWUI. */
private const val USE_TEXTURE_VIEW = true

/**
 * Builds an OnTouchListener for X11 views with velocity-scaled touch tracking.
 *
 * Uses X11 window hit-testing for scroll vs. widget disambiguation:
 * - On DOWN: inject X11 touch and check if the touch hits a widget (knob, slider, button).
 *   If widget → claim touch immediately (block parent). If background → let LazyColumn scroll.
 * - On CANCEL: LazyColumn intercepted — release X11 button.
 */
private fun buildX11TouchListener(
    displayNumber: Int
): View.OnTouchListener {
    var lastRawX = 0f
    var lastRawY = 0f
    var accumulatedX = 0f
    var accumulatedY = 0f
    @Suppress("VARIABLE_WITH_REDUNDANT_INITIALIZER") var claimedForWidget = false

    return View.OnTouchListener { view, event ->
        when (event.action) {
            MotionEvent.ACTION_DOWN -> {
                lastRawX = event.x
                lastRawY = event.y
                accumulatedX = event.x
                accumulatedY = event.y
                X11DisplayManager.updateLastTouchTime(SystemClock.elapsedRealtimeNanos())
                X11Bridge.injectTouch(displayNumber, 0, accumulatedX.toInt(), accumulatedY.toInt())
                X11Bridge.requestX11Frame(displayNumber)
                claimedForWidget = X11Bridge.isWidgetAtPoint(displayNumber, event.x.toInt(), event.y.toInt())
                if (claimedForWidget) {
                    view.parent?.requestDisallowInterceptTouchEvent(true)
                }
                true
            }
            MotionEvent.ACTION_MOVE -> {
                val deltaX = event.x - lastRawX
                val deltaY = event.y - lastRawY
                accumulatedX += deltaX * VELOCITY_SCALE
                accumulatedY += deltaY * VELOCITY_SCALE
                lastRawX = event.x
                lastRawY = event.y
                X11DisplayManager.updateLastTouchTime(SystemClock.elapsedRealtimeNanos())
                X11Bridge.injectTouch(displayNumber, 2, accumulatedX.toInt(), accumulatedY.toInt())
                X11Bridge.requestX11Frame(displayNumber)
                true
            }
            MotionEvent.ACTION_UP -> {
                X11DisplayManager.updateLastTouchTime(SystemClock.elapsedRealtimeNanos())
                X11Bridge.injectTouch(displayNumber, 1, accumulatedX.toInt(), accumulatedY.toInt())
                X11Bridge.requestX11Frame(displayNumber)
                view.parent?.requestDisallowInterceptTouchEvent(false)
                claimedForWidget = false
                true
            }
            MotionEvent.ACTION_CANCEL -> {
                X11Bridge.injectTouch(displayNumber, 1, accumulatedX.toInt(), accumulatedY.toInt())
                X11Bridge.requestX11Frame(displayNumber)
                claimedForWidget = false
                true
            }
            else -> false
        }
    }
}

/**
 * Embeds the native X11 display (EGL + ANativeWindow) and loads the plugin's
 * native X11 UI. One Surface per plugin; the native X server listens on
 * 127.0.0.1:(6000+displayNumber) and renders via EGL to the surface.
 * Touch is injected as X11 pointer events (MotionNotify, ButtonPress/Release).
 *
 * @param isVisible If true, the X11 display is visible and rendering. If false, rendering is paused.
 *                  Used when switching between UI modes to avoid destroying/recreating the display.
 * @param shouldDestroyOnDispose If true, the X11 display will be fully destroyed when this
 * composable is disposed (e.g., when removing plugin from rack). If false, only rendering is paused.
 */
@Composable
fun PluginX11UiView(
    pluginIndex: Int,
    displayNumber: Int,
    isVisible: Boolean = true,
    modifier: Modifier = Modifier,
    shouldDestroyOnDispose: Boolean = true,
    onUiReady: () -> Unit = {},
    onUiError: (String) -> Unit = {},
    onPluginSizeKnown: (width: Int, height: Int, uiScale: Float) -> Unit = { _, _, _ -> }
) {
    Log.i("AudioLifecycle", "PluginX11UiView composed pluginIndex=$pluginIndex displayNumber=$displayNumber")
    // Capture pluginIndex by reference so it can update without triggering DisposableEffect
    val currentPluginIndex by rememberUpdatedState(pluginIndex)
    var isReady by remember { mutableStateOf(false) }
    var surfaceViewRef by remember { mutableStateOf<View?>(null) }
    /** Set when surfaceDestroyed deferred (createPluginUI still running); cleanup when executor's createPluginUI returns */
    var pendingDetachDisplayNumber by remember { mutableStateOf<Int?>(null) }
    /** Set to true when surface destruction is in progress - prevents destroyPluginUI from running in onDispose */
    var isSurfaceDestroyed by remember { mutableStateOf(false) }

    // Handle visibility changes - pause/resume rendering
    LaunchedEffect(isVisible) {
        if (isVisible) {
            Log.i(TAG, "[LIFECYCLE] PluginX11UiView becoming visible - resuming display=$displayNumber")
            // The display may not exist yet if this fires before onSurfaceTextureAvailable.
            // resumeX11Display is a safe no-op when the display doesn't exist.
            X11Bridge.resumeX11Display(displayNumber)
            // Pump frame requests with enough iterations to cover the createPluginUI delay
            // (X11_INIT_DELAY_MS + instantiation time). requestFrame() auto-restarts the
            // render thread if it died, so this also recovers from screen-off/on EGL failures.
            repeat(30) {
                X11Bridge.requestX11Frame(displayNumber)
                delay(50)
            }
        } else {
            Log.i(TAG, "[LIFECYCLE] PluginX11UiView becoming hidden - pausing display=$displayNumber")
            X11Bridge.hideX11Display(displayNumber)
        }
    }

    // Low-frequency fallback frame request as safety net (rendering is primarily damage-driven via PutImage).
    // requestFrame() auto-restarts dead render threads, so this also serves as a heartbeat.
    LaunchedEffect(isVisible) {
        if (isVisible) {
            while (isActive) {
                X11Bridge.requestX11Frame(displayNumber)
                delay(500)
            }
        }
    }

    DisposableEffect(displayNumber, shouldDestroyOnDispose) {
        Log.i("AudioLifecycle", "[LIFECYCLE] PluginX11UiView DisposableEffect ENTERED pluginIndex=$pluginIndex displayNumber=$displayNumber shouldDestroyOnDispose=$shouldDestroyOnDispose isAllocated=${X11DisplayManager.isDisplayAllocated(displayNumber)}")
        onDispose {
            surfaceViewRef = null
            Log.i("AudioLifecycle", "[LIFECYCLE] PluginX11UiView DisposableEffect.onDispose plugin=$currentPluginIndex display=$displayNumber shouldDestroyOnDispose=$shouldDestroyOnDispose thread=${Thread.currentThread().name} isAllocated=${X11DisplayManager.isDisplayAllocated(displayNumber)} isSurfaceDestroyed=$isSurfaceDestroyed")

            if (shouldDestroyOnDispose && !isSurfaceDestroyed) {
                // Full cleanup when removing plugin - run off main thread to avoid ANR
                Log.i(TAG, "[LIFECYCLE] Destroying plugin UI (shouldDestroyOnDispose=true)")
                val pIdx = currentPluginIndex
                X11DisplayManager.teardownExecutor.execute { X11Bridge.destroyPluginUI(pIdx) }
            } else if (!shouldDestroyOnDispose) {
                Log.i(TAG, "[LIFECYCLE] Keeping X11 display alive (shouldDestroyOnDispose=false)")
            } else {
                Log.i(TAG, "[LIFECYCLE] Skipping destroyPluginUI in onDispose - surface destruction in progress, will cleanup in deferred task")
            }
            // Do NOT detach/destroy/release here — surfaceDestroyed will run when the view is
            // torn down; detaching here while HWUI may still use the view causes "pthread_mutex_lock
            // called on a destroyed mutex" in hwuiTask0. Release is done in surfaceDestroyed.
        }
    }

    // REMOVED: Continuous frame request loop - it was causing unnecessary overhead
    // and conflicting with touch-triggered frame requests. Now frames are only
    // requested when touch events occur or when the plugin updates the UI.

    key(displayNumber) {
        AndroidView(
        factory = { ctx ->
            Log.i(TAG, "AndroidView factory: plugin $pluginIndex display :$displayNumber (USE_TEXTURE_VIEW=$USE_TEXTURE_VIEW)")
            Log.i(TAG, "Display :$displayNumber -> port ${X11DisplayManager.getPort(displayNumber)}")

            var attached = false
            /** True if we created the Surface (TextureView path); then we must release it on destroy. */
            var weOwnSurface = false

            fun doAttachAndCreate(surface: Surface, width: Int, height: Int) {
                if (attached || width < 100 || height < 100) return
                attached = true
                Log.i("AudioLifecycle", "PluginX11UiView attach display=$displayNumber ${width}x$height -> attaching")
                val rootId = X11Bridge.attachSurfaceToDisplay(displayNumber, surface, width, height)
                if (rootId == 0L) {
                    Log.e(TAG, "attachSurfaceToDisplay failed for plugin $pluginIndex")
                    onUiError("Failed to attach surface")
                    attached = false
                    return
                }
                Log.i("AudioLifecycle", "PluginX11UiView attachSurfaceToDisplay ok rootId=$rootId -> nativeBeginCreatePluginUI then create")
                X11Bridge.beginCreatePluginUI(displayNumber, pluginIndex)
                // Each plugin uses its own X11 display + pluginUI thread, so
                // createPluginUI calls don't need to be serialized.  Running them
                // on separate threads avoids the single-threaded executor
                // bottleneck where one slow/hung plugin blocks all others.
                Thread({
                    Log.i("AudioLifecycle", "PluginX11UiView create thread STARTED display=$displayNumber (ensureX11LibsDir, sleep ${X11_INIT_DELAY_MS}ms, then createPluginUI)")
                    X11Bridge.ensureX11LibsDir(ctx)
                    Thread.sleep(X11_INIT_DELAY_MS)
                    Log.i("AudioLifecycle", "createPluginUI START pluginIndex=$pluginIndex display=$displayNumber rootId=$rootId thread=${Thread.currentThread().name}")
                    val ok = X11Bridge.createPluginUI(pluginIndex, displayNumber, rootId)
                    Log.i("AudioLifecycle", "createPluginUI DONE pluginIndex=$pluginIndex ok=$ok thread=${Thread.currentThread().name}")
                    Handler(Looper.getMainLooper()).postDelayed({
                        Log.i("X11Debug", "createPluginUI done callback (1ms) thread=${Thread.currentThread().name} ok=$ok pendingDetach=$pendingDetachDisplayNumber")
                        if (ok) {
                            isReady = true
                            onUiReady()
                            val sizeArr = X11Bridge.getX11PluginSize(displayNumber)
                            val uiScale = X11Bridge.getX11UIScale(displayNumber)
                            if (sizeArr[0] > 0 && sizeArr[1] > 0) {
                                onPluginSizeKnown(sizeArr[0], sizeArr[1], uiScale)
                            }
                        } else {
                            Log.e(TAG, "Failed to instantiate X11 UI for plugin $pluginIndex")
                            onUiError("Failed to instantiate native plugin UI")
                        }
                        val toClean = pendingDetachDisplayNumber
                        if (toClean != null) {
                            pendingDetachDisplayNumber = null
                            val displayToClean = toClean
                            Log.i(TAG, "[LIFECYCLE] createPluginUI done: scheduling DEFERRED cleanup in 2000ms (display=$displayToClean)")
                            Handler(Looper.getMainLooper()).postDelayed({
                                Log.i(TAG, "[LIFECYCLE] DEFERRED 2000ms task RUNNING display=$displayToClean thread=${Thread.currentThread().name} -> destroyPluginUI then detachAndDestroy")
                                try {
                                    // First destroy the plugin UI (since we skipped it in onDispose)
                                    // This must happen before detaching the display to ensure proper cleanup order
                                    X11Bridge.destroyPluginUI(currentPluginIndex)
                                    // Then detach and destroy the display
                                    X11Bridge.detachAndDestroyX11DisplayIfExists(displayToClean)
                                    // Release display number AFTER the display is fully destroyed
                                    X11DisplayManager.releaseDisplay(displayToClean)
                                    Log.i(TAG, "[LIFECYCLE] DEFERRED cleanup COMPLETED display=$displayToClean")
                                } catch (t: Throwable) {
                                    Log.e(TAG, "[LIFECYCLE] DEFERRED cleanup FAILED display=$displayToClean", t)
                                    // Still release the display on failure to avoid leaking it
                                    X11DisplayManager.releaseDisplay(displayToClean)
                                }
                            }, X11_CLEANUP_DELAY_MS)
                        } else {
                            Log.i(TAG, "[LIFECYCLE] createPluginUI done: no pendingDetach, no deferred cleanup needed")
                        }
                    }, 1)
                }, "X11Create-$displayNumber").apply { isDaemon = true }.start()
            }

            fun doSurfaceDestroyed() {
                if (attached) {
                    Log.i(TAG, "[LIFECYCLE] surfaceDestroyed ENTER display=$displayNumber thread=${Thread.currentThread().name} isAllocated=${X11DisplayManager.isDisplayAllocated(displayNumber)}")
                    attached = false
                    isReady = false
                    isSurfaceDestroyed = true  // Mark that surface destruction is in progress
                    // NOTE: Do NOT stop audio engine here. The audio engine shares mutexes
                    // with HWUI threads through the AAudio/Oboe driver. Stopping it destroys
                    // those mutexes and crashes HWUI. The audio engine should keep running.
                    // CRITICAL: Stop render thread BEFORE releasing surface to prevent
                    // "pthread_mutex_lock on destroyed mutex" crash. The render thread
                    // must exit before we release the ANativeWindow to avoid eglSwapBuffers
                    // being called on a destroyed surface.
                    X11Bridge.stopX11RenderThreadOnly(displayNumber)
                    // CRITICAL: Do NOT release the Surface/SurfaceTexture here when hiding.
                    // Releasing it triggers Android graphics driver cleanup that destroys
                    // shared mutexes with HWUI threads. We keep the surface alive and only
                    // release it when the plugin is actually removed (in DisposableEffect).
                    if (weOwnSurface) {
                        // Store reference for later cleanup instead of releasing now
                        Log.i(TAG, "[LIFECYCLE] Keeping surface alive for display=$displayNumber (will release on plugin removal)")
                    }
                    // NOTE: We intentionally do NOT call signalDetachSurfaceFromDisplay here.
                    // That would close the X11 connection and destroy shared driver mutexes.
                    // The X server keeps running so we can reattach later.
                    Log.i(TAG, "[LIFECYCLE] surfaceDestroyed EXIT display=$displayNumber (surface and X11 connection kept alive)")
                }
            }

            if (USE_TEXTURE_VIEW) {
                val textureView = TextureView(ctx).apply {
                    surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                        override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
                            Log.i("AudioLifecycle", "PluginX11UiView TextureView onSurfaceTextureAvailable display=$displayNumber ${width}x$height")
                            weOwnSurface = true
                            val surface = Surface(texture)
                            doAttachAndCreate(surface, width, height)
                        }
                        override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) {
                            if (width > 0 && height > 0) {
                                X11Bridge.setSurfaceSize(displayNumber, width, height)
                                // When switching to X11, the view may have been created with 0 height (other UI mode).
                                // Attach and create native UI when we first get a valid size (e.g. after layout with X11 visible).
                                if (!attached && width >= 100 && height >= 100) {
                                    weOwnSurface = true
                                    val surface = Surface(texture)
                                    doAttachAndCreate(surface, width, height)
                                }
                            }
                        }
                        override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
                            // CRITICAL: Defer surface destruction to avoid race with HWUI.
                            // Returning false allows the system to release the SurfaceTexture,
                            // but we defer our cleanup to avoid the "pthread_mutex_lock on
                            // destroyed mutex" crash in HWUI threads.
                            doSurfaceDestroyed()
                            return false  // Return false - system will release, we defer cleanup
                        }
                        override fun onSurfaceTextureUpdated(texture: SurfaceTexture) {}
                    }
                    setOnTouchListener(buildX11TouchListener(displayNumber))
                }
                surfaceViewRef = textureView
                return@AndroidView textureView
            }

            val surfaceView = SurfaceView(ctx).apply {
                holder.addCallback(object : android.view.SurfaceHolder.Callback {
                    override fun surfaceCreated(holder: android.view.SurfaceHolder) {
                        Log.i("AudioLifecycle", "PluginX11UiView surfaceCreated display=$displayNumber (waiting for surfaceChanged with size)")
                    }
                    override fun surfaceChanged(holder: android.view.SurfaceHolder, format: Int, width: Int, height: Int) {
                        if (width > 0 && height > 0) X11Bridge.setSurfaceSize(displayNumber, width, height)
                        doAttachAndCreate(holder.surface, width, height)
                    }
                    override fun surfaceDestroyed(holder: android.view.SurfaceHolder) {
                        doSurfaceDestroyed()
                    }
                })
                
                setOnTouchListener(buildX11TouchListener(displayNumber))
            }
            surfaceViewRef = surfaceView
            surfaceView
        },
        update = { surfaceViewRef = it },
        modifier = modifier.fillMaxSize()
    )
    }
}
