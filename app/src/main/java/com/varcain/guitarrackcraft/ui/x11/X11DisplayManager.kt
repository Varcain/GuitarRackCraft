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

package com.varcain.guitarrackcraft.ui.x11

import android.os.Handler
import android.os.Looper
import android.util.Log
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.asCoroutineDispatcher
import java.util.concurrent.Executor
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicLong

/**
 * Manages X11 display numbers for plugin UIs.
 * Each plugin UI gets its own X11 display (e.g., :10, :11, etc.)
 * The X11 protocol maps display :N to TCP port 6000+N.
 *
 * [pluginUiDispatcher] is a single-thread dispatcher used for all native X11 UI work
 * (create + idle). Xlib requires all use of a Display to be from one thread.
 *
 * [teardownExecutor] is a dedicated thread for X11 display teardown (detachAndDestroy).
 * We never run teardown on the plugin UI executor so that when executor == main thread we
 * don't close the X connection from the same thread that just ran the plugin.
 */
object X11DisplayManager {
    private val _pluginUiExecutor = Executors.newSingleThreadExecutor { r ->
        Thread(r, "PluginX11UI").apply {
            isDaemon = true
            priority = Thread.NORM_PRIORITY + 2
        }
    }

    /** Single-thread executor for X11/plugin UI work. Use for direct submit of idle + postInvalidate (touch path). */
    val pluginUiExecutor: Executor get() = _pluginUiExecutor

    /** Dedicated thread for display teardown only. Use for deferred detachAndDestroy so we never run it on the plugin UI thread. */
    private val _teardownExecutor = Executors.newSingleThreadExecutor { r ->
        Thread(r, "X11Teardown").apply { isDaemon = true }
    }
    val teardownExecutor: Executor get() = _teardownExecutor

    /** Single thread for createPluginUI and idlePluginUIs so Xlib is used from one thread only. */
    val pluginUiDispatcher: CoroutineDispatcher = _pluginUiExecutor.asCoroutineDispatcher()

    private val lastTouchEventNs = AtomicLong(0L)

    /** Call from touch listener so idle loop can use 4 ms delay while user is interacting. */
    fun updateLastTouchTime(ns: Long) { lastTouchEventNs.set(ns) }

    fun getLastTouchTimeNs(): Long = lastTouchEventNs.get()

    private const val TAG = "X11DisplayManager"
    private const val DISPLAY_BASE = 10  // Start at :10 (port 6010)
    private const val MAX_DISPLAYS = 50

    private val allocatedDisplays = mutableSetOf<Int>()

    /**
     * Allocate a new X11 display number.
     * @return Display number (e.g., 10 for display :10), or -1 if none available
     */
    @Synchronized
    fun allocateDisplay(): Int {
        for (i in DISPLAY_BASE until (DISPLAY_BASE + MAX_DISPLAYS)) {
            if (i !in allocatedDisplays) {
                allocatedDisplays.add(i)
                Log.i(TAG, "[DISPLAY-LIFECYCLE] ALLOCATED display :$i (port ${6000 + i}) - allocated set size: ${allocatedDisplays.size}")
                return i
            }
        }
        Log.e(TAG, "[DISPLAY-LIFECYCLE] FAILED to allocate display - no available displays! allocated set size: ${allocatedDisplays.size}")
        return -1
    }

    /**
     * Release a display number back to the pool.
     * IMPORTANT: Only call this AFTER nativeDetachAndDestroyX11DisplayIfExists() has completed.
     * Calling this before the display is fully destroyed can cause race conditions where
     * a new plugin gets the same display number while the old one is still being torn down.
     */
    @Synchronized
    fun releaseDisplay(displayNumber: Int) {
        val wasAllocated = displayNumber in allocatedDisplays
        if (allocatedDisplays.remove(displayNumber)) {
            Log.i(TAG, "[DISPLAY-LIFECYCLE] RELEASED display :$displayNumber - was allocated: $wasAllocated, remaining allocated: ${allocatedDisplays.size}")
        } else {
            Log.w(TAG, "[DISPLAY-LIFECYCLE] ATTEMPTED TO RELEASE display :$displayNumber but it was NOT allocated! allocated set size: ${allocatedDisplays.size}")
        }
    }

    /**
     * Check if a display number is currently allocated.
     */
    @Synchronized
    fun isDisplayAllocated(displayNumber: Int): Boolean {
        return displayNumber in allocatedDisplays
    }

    /**
     * Get the TCP port for a display number.
     */
    fun getPort(displayNumber: Int): Int = 6000 + displayNumber

    /**
     * Get display string (e.g., ":10").
     */
    fun getDisplayString(displayNumber: Int): String = ":$displayNumber"
}
