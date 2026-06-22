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

package com.varcain.guitarrackcraft.engine

import android.content.Context
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.GLES20
import android.os.Build
import android.util.Log

/**
 * The plugin-editor Vulkan renderer the wine host should use.
 *
 *  - [TURNIP]   — Mesa Turnip on the Adreno GPU (hardware). Default env (no
 *                 VSTPOC_FORCE_LAVAPIPE). Only available on Adreno devices.
 *  - [LAVAPIPE] — Mesa lavapipe software Vulkan (llvmpipe). Universal fallback,
 *                 selected by VSTPOC_FORCE_LAVAPIPE=1 in cache/wine_env.txt.
 */
enum class Renderer(val label: String) {
    TURNIP("Turnip (GPU)"),
    LAVAPIPE("Lavapipe (SW)")
}

/**
 * Detects whether the device GPU is Adreno (Qualcomm). The result is cached for
 * the process lifetime (the GPU never changes), so the EGL probe runs at most
 * once. Turnip needs an Adreno GPU; everything else must use lavapipe.
 */
object GpuDetector {
    private const val TAG = "GpuDetector"

    @Volatile private var cached: Boolean? = null

    /** True iff the GPU reports as Adreno/Qualcomm. Safe default on failure = false. */
    fun isAdreno(): Boolean {
        cached?.let { return it }
        return synchronized(this) { cached ?: probe().also { cached = it } }
    }

    private fun probe(): Boolean {
        // GL_RENDERER/GL_VENDOR is authoritative.
        val gl = runCatching { eglRendererString() }.getOrNull()
        if (gl != null) {
            val s = gl.lowercase()
            return s.contains("adreno") || s.contains("qualcomm")
        }
        // EGL probe failed — fall back to a cheap board hint.
        val hw = (Build.HARDWARE + " " + Build.BOARD).lowercase()
        Log.w(TAG, "EGL probe failed; falling back to Build hints '$hw'")
        return hw.contains("qcom") || hw.contains("qualcomm")
    }

    /** Surfaceless EGL PBuffer context → glGetString(GL_RENDERER)+GL_VENDOR. */
    private fun eglRendererString(): String {
        val display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        check(display != EGL14.EGL_NO_DISPLAY) { "no EGL display" }
        val ver = IntArray(2)
        check(EGL14.eglInitialize(display, ver, 0, ver, 1)) { "eglInitialize failed" }

        var context = EGL14.EGL_NO_CONTEXT
        var surface = EGL14.EGL_NO_SURFACE
        try {
            val configs = arrayOfNulls<EGLConfig>(1)
            val num = IntArray(1)
            val ok = EGL14.eglChooseConfig(
                display,
                intArrayOf(
                    EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                    EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                    EGL14.EGL_RED_SIZE, 8,
                    EGL14.EGL_GREEN_SIZE, 8,
                    EGL14.EGL_BLUE_SIZE, 8,
                    EGL14.EGL_NONE
                ),
                0, configs, 0, 1, num, 0
            )
            check(ok && num[0] > 0 && configs[0] != null) { "eglChooseConfig failed" }
            val config = configs[0]

            context = EGL14.eglCreateContext(
                display, config, EGL14.EGL_NO_CONTEXT,
                intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0
            )
            check(context != EGL14.EGL_NO_CONTEXT) { "eglCreateContext failed" }

            surface = EGL14.eglCreatePbufferSurface(
                display, config,
                intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE), 0
            )
            check(surface != EGL14.EGL_NO_SURFACE) { "eglCreatePbufferSurface failed" }
            check(EGL14.eglMakeCurrent(display, surface, surface, context)) { "eglMakeCurrent failed" }

            val renderer = GLES20.glGetString(GLES20.GL_RENDERER) ?: ""
            val vendor = GLES20.glGetString(GLES20.GL_VENDOR) ?: ""
            Log.i(TAG, "GL_RENDERER='$renderer' GL_VENDOR='$vendor'")
            return "$renderer $vendor"
        } finally {
            EGL14.eglMakeCurrent(
                display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT
            )
            if (surface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, surface)
            if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context)
            // NOTE: deliberately NOT calling eglTerminate(display) — EGL_DEFAULT_DISPLAY
            // is shared with the system/Compose; terminating it can invalidate their
            // contexts. Leaving it initialized is harmless (the system keeps it so).
        }
    }
}

/**
 * Persists the user's plugin-editor renderer choice (one global setting).
 *
 * The default depends on the GPU: Adreno → [Renderer.TURNIP], otherwise
 * [Renderer.LAVAPIPE]. On a non-Adreno device Turnip can't run, so [getRenderer]
 * always returns LAVAPIPE there (coercing any stale stored TURNIP).
 */
object RendererPreferenceManager {
    private const val PREFS_NAME = "renderer_preferences"
    private const val KEY = "renderer"

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun getRenderer(context: Context): Renderer {
        // Non-Adreno: only lavapipe is possible.
        if (!GpuDetector.isAdreno()) return Renderer.LAVAPIPE

        val stored = prefs(context).getString(KEY, null)?.let {
            runCatching { Renderer.valueOf(it) }.getOrNull()
        }
        return stored ?: Renderer.TURNIP
    }

    fun setRenderer(context: Context, renderer: Renderer) {
        prefs(context).edit().putString(KEY, renderer.name).apply()
    }
}
