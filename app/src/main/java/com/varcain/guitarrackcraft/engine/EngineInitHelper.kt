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
import android.util.Log
import com.varcain.guitarrackcraft.BuildConfig
import java.io.File

/**
 * Shared engine initialization logic used by both MainActivity and X11PluginUIActivity.
 */
object EngineInitHelper {

    private const val TAG = "EngineInitHelper"

    /**
     * Preload lilv shared library by absolute path so that when libguitarrackcraft.so
     * is loaded the linker finds it (DT_NEEDED liblilv-0.so.0).
     */
    fun preloadLilv(nativeLibDir: String?) {
        if (nativeLibDir == null) return
        for (name in listOf("liblilv-0.so.0", "liblilv-0.so")) {
            val f = File(nativeLibDir, name)
            if (f.canRead()) {
                try {
                    System.load(f.absolutePath)
                    Log.d(TAG, "Preloaded $name")
                    break
                } catch (e: Throwable) {
                    Log.w(TAG, "Preload $name failed: ${e.message}")
                }
            }
        }
    }

    /**
     * Initialize the native engine with LV2 path, native lib dir, and files dir.
     * For playstore flavor, extracts plugin .so from PAD packs first.
     * Returns true if initialization succeeded.
     */
    fun initEngine(
        context: Context,
        onExtractionProgress: ((extracted: Int, total: Int) -> Unit)? = null
    ): Boolean {
        val engine = NativeEngine.getInstance()
        val lv2Dir = File(context.applicationContext.filesDir, "lv2")
        val lv2Path = lv2Dir.canonicalPath
        Log.d(TAG, "Setting LV2 path: $lv2Path")
        engine.nativeSetLv2Path(lv2Path)
        engine.nativeSetNativeLibDir(context.applicationInfo.nativeLibraryDir ?: "")
        engine.nativeSetFilesDir(context.applicationContext.filesDir.absolutePath)

        // For playstore flavor: extract plugin .so from PAD packs before init
        if (BuildConfig.USE_ASSET_PACKS) {
            PluginAssetExtractor.ensurePluginsExtracted(context, onExtractionProgress)
            val pluginDir = PluginAssetExtractor.getPluginDir(context)
            engine.nativeSetPluginLibDir(pluginDir.absolutePath)
            Log.d(TAG, "Plugin lib dir (PAD): ${pluginDir.absolutePath}")
        }

        // Preload X11/Mesa SONAME libs (libX11.so.6, libGL.so.1, etc.)
        // BEFORE nativeInit(), because some DSP plugins (e.g. full AIDA-X via DPF)
        // link against libX11.so.6 and lilv's dlopen will fail without it.
        engine.ensureX11LibsDir(context)
        val ok = engine.nativeInit()
        if (!ok) {
            Log.e(TAG, "Failed to initialize native engine")
        } else {
            val count = engine.getAvailablePlugins().size
            Log.d(TAG, "Native engine init OK, plugin count=$count")
        }
        return ok
    }
}
