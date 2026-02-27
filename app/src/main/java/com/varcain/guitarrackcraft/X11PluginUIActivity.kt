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

package com.varcain.guitarrackcraft

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Choreographer
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.activity.ComponentActivity
import com.varcain.guitarrackcraft.engine.EngineInitHelper
import com.varcain.guitarrackcraft.engine.RackManager
import com.varcain.guitarrackcraft.engine.X11Bridge
import com.varcain.guitarrackcraft.ui.x11.X11DisplayManager

/**
 * Hosts the X11 plugin UI in a **separate process** (:x11ui) so EGL/GL state is not shared
 * with the main process HWUI, avoiding "pthread_mutex_lock on destroyed mutex" crashes.
 *
 * Started by the main app with EXTRA_PLUGIN_ID (plugin fullId, e.g. "LV2:http://..."). This
 * process inits its own native engine (no audio), adds the plugin to the rack, and runs
 * createPluginUI on a Surface in this activity. Parameter changes are not yet synced back
 * to the main process.
 */
class X11PluginUIActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        pluginId = intent.getStringExtra(EXTRA_PLUGIN_ID)
        if (pluginId.isNullOrEmpty()) {
            Log.e(TAG, "Missing $EXTRA_PLUGIN_ID")
            finish()
            return
        }
        Log.i(TAG, "onCreate process=${android.os.Process.myPid()} pluginId=$pluginId")
        initNativeInThisProcess()
        var touchLogCount = 0
        val surfaceView = SurfaceView(this).apply {
            setOnTouchListener { _, event ->
                val action = when (event.action) {
                    android.view.MotionEvent.ACTION_DOWN -> 0
                    android.view.MotionEvent.ACTION_UP -> 1
                    android.view.MotionEvent.ACTION_MOVE -> 2
                    else -> -1
                }
                if (action >= 0 && displayNumber >= 0) {
                    if (++touchLogCount <= 40 || touchLogCount % 50 == 0) {
                        Log.i(TAG, "X11Touch display=$displayNumber action=$action x=${event.x.toInt()} y=${event.y.toInt()} (#$touchLogCount)")
                    }
                    X11Bridge.injectTouch(displayNumber, action, event.x.toInt(), event.y.toInt())
                    X11DisplayManager.pluginUiExecutor.execute {
                        X11Bridge.idlePluginUIs()
                        X11Bridge.requestX11Frame(displayNumber)
                    }
                }
                true
            }
        }
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {}
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                if (attached || width < 10 || height < 10) {
                    if (width > 0 && height > 0 && displayNumber >= 0) X11Bridge.setSurfaceSize(displayNumber, width, height)
                    return
                }
                attached = true
                Log.i(TAG, "surfaceChanged ${width}x$height -> attach and createPluginUI")
                val rootId = X11Bridge.attachSurfaceToDisplay(displayNumber, holder.surface, width, height)
                if (rootId == 0L) {
                    Log.e(TAG, "attachSurfaceToDisplay failed")
                    finish()
                    return
                }
                X11Bridge.beginCreatePluginUI(displayNumber, 0)
                X11DisplayManager.pluginUiExecutor.execute {
                    X11Bridge.ensureX11LibsDir(this@X11PluginUIActivity)
                    Thread.sleep(X11_INIT_DELAY_MS)
                    val ok = X11Bridge.createPluginUI(0, displayNumber, rootId)
                    Handler(Looper.getMainLooper()).post {
                        if (!ok) {
                            Log.e(TAG, "createPluginUI failed")
                            finish()
                        } else {
                            startFrameAndIdleLoops()
                        }
                    }
                }
            }
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                if (attached) {
                    attached = false
                    X11Bridge.signalDetachSurfaceFromDisplay(displayNumber)
                    pendingDetach = true
                    Log.i(TAG, "surfaceDestroyed -> deferred cleanup")
                }
            }
        })
        surfaceView.contentDescription = "x11_plugin_viewport"
        setContentView(surfaceView)
    }

    private fun initNativeInThisProcess() {
        EngineInitHelper.preloadLilv(applicationInfo.nativeLibraryDir)
        if (!EngineInitHelper.initEngine(this)) {
            Log.e(TAG, "nativeInit failed")
            finish()
            return
        }
        val idx = RackManager.addPlugin(pluginId!!, 0)
        if (idx < 0) {
            Log.e(TAG, "addPluginToRack failed for $pluginId")
            finish()
            return
        }
        displayNumber = X11DisplayManager.allocateDisplay()
        if (displayNumber < 0) {
            Log.e(TAG, "No X11 display available")
            finish()
            return
        }
        Log.i(TAG, "Subprocess inited: plugin at index $idx, display $displayNumber")
    }

    override fun onDestroy() {
        stopFrameAndIdleLoops()
        if (pendingDetach && displayNumber >= 0) {
            X11Bridge.detachAndDestroyX11DisplayIfExists(displayNumber)
            X11DisplayManager.releaseDisplay(displayNumber)
        }
        super.onDestroy()
    }

    /** Pump EGL swaps and plugin idle so the X11 UI is visible and responsive. */
    private fun startFrameAndIdleLoops() {
        if (displayNumber < 0) return
        idleScheduled = true
        val choreographer = Choreographer.getInstance()
        frameCallback = object : Choreographer.FrameCallback {
            override fun doFrame(frameTimeNanos: Long) {
                if (displayNumber >= 0 && !isFinishing() && frameCallback != null) {
                    Handler(Looper.getMainLooper()).post { X11Bridge.requestX11Frame(displayNumber) }
                    choreographer.postFrameCallback(this)
                }
            }
        }
        choreographer.postFrameCallback(frameCallback!!)
        X11Bridge.requestX11Frame(displayNumber)
        scheduleIdleLoop()
        Log.d(TAG, "Started frame and idle loops for display $displayNumber")
    }

    private fun stopFrameAndIdleLoops() {
        frameCallback = null
        idleScheduled = false
    }

    private fun scheduleIdleLoop() {
        if (!idleScheduled || isFinishing() || displayNumber < 0) return
        X11DisplayManager.pluginUiExecutor.execute {
            if (!idleScheduled || isFinishing()) return@execute
            X11Bridge.idlePluginUIs()
            Handler(Looper.getMainLooper()).postDelayed({
                if (idleScheduled && !isFinishing() && displayNumber >= 0) scheduleIdleLoop()
            }, 8)
        }
    }

    companion object {
        const val EXTRA_PLUGIN_ID = "com.varcain.guitarrackcraft.extra.PLUGIN_ID"
        private const val TAG = "X11PluginUIActivity"
        private const val X11_INIT_DELAY_MS = 400L
    }

    private var pluginId: String? = null
    private var displayNumber: Int = -1
    private var attached: Boolean = false
    private var pendingDetach: Boolean = false
    private var frameCallback: Choreographer.FrameCallback? = null
    private var idleScheduled: Boolean = false
}
