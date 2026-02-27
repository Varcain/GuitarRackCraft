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
import java.io.File

/**
 * Extracts plugin .so files from Play Asset Delivery install-time packs to a
 * dlopen-able directory on first launch (or after app update).
 *
 * Install-time PAD packs are merged into the app's asset manager, so their
 * contents are accessible via context.assets.  Plugin .so files are stored
 * under "plugins/arm64-v8a/" in each pack.
 *
 * A build-time manifest (plugin_libs.txt) lists all plugin .so filenames so
 * extraction does not depend on assets.list() (which is unreliable across
 * split APKs on some Android versions).
 *
 * For the "full" flavor (GitHub APK), all plugin .so files are in jniLibs
 * and this class is never called.
 */
object PluginAssetExtractor {

    private const val TAG = "PluginAssetExtractor"
    private const val PLUGINS_ASSET_DIR = "plugins/arm64-v8a"
    private const val MANIFEST_FILE = "plugin_libs.txt"
    private const val VERSION_STAMP_FILE = ".version_code"

    /**
     * Directory where extracted plugin .so files are stored.
     * This path is passed to native code as pluginLibDir.
     */
    fun getPluginDir(context: Context): File =
        File(context.filesDir, "plugins/arm64-v8a")

    /**
     * Read plugin .so filenames from the build-time manifest (plugin_libs.txt).
     * Falls back to assets.list() if manifest is missing.
     */
    private fun getPluginFileNames(context: Context): List<String> {
        // Primary: read build-time manifest
        try {
            val names = context.assets.open(MANIFEST_FILE).bufferedReader().use { reader ->
                reader.readLines().map { it.trim() }.filter { it.endsWith(".so") }
            }
            if (names.isNotEmpty()) {
                Log.d(TAG, "Read ${names.size} entries from $MANIFEST_FILE")
                return names
            }
        } catch (e: Exception) {
            Log.w(TAG, "$MANIFEST_FILE not found, falling back to assets.list(): ${e.message}")
        }

        // Fallback: enumerate via assets.list()
        return try {
            val listed = context.assets.list(PLUGINS_ASSET_DIR)
                ?.filter { it.endsWith(".so") } ?: emptyList()
            Log.d(TAG, "assets.list($PLUGINS_ASSET_DIR) returned ${listed.size} entries")
            listed
        } catch (e: Exception) {
            Log.e(TAG, "Failed to list plugin assets: ${e.message}")
            emptyList()
        }
    }

    /**
     * Ensure plugins are extracted.  Skips extraction if already done for the
     * current version code, but always pre-loads DSP .so via System.load().
     * @param onProgress optional callback: (extracted, total) counts
     * @return true if extraction completed (or was already up to date)
     */
    fun ensurePluginsExtracted(
        context: Context,
        onProgress: ((extracted: Int, total: Int) -> Unit)? = null
    ): Boolean {
        val pluginDir = getPluginDir(context)
        pluginDir.mkdirs()

        val currentVersion = try {
            context.packageManager
                .getPackageInfo(context.packageName, 0)
                .longVersionCode
        } catch (_: Exception) {
            0L
        }

        val assetNames = getPluginFileNames(context)

        if (assetNames.isEmpty()) {
            Log.w(TAG, "No plugin .so files found (no manifest and assets.list empty)")
            return true
        }

        // Stamp format: "<versionCode>:<count>"
        var needsExtraction = true
        val stampFile = File(pluginDir, VERSION_STAMP_FILE)
        if (stampFile.exists()) {
            val parts = stampFile.readText().trim().split(":")
            val stampedVersion = parts.getOrNull(0)?.toLongOrNull() ?: 0L
            val stampedCount = parts.getOrNull(1)?.toIntOrNull() ?: 0
            if (stampedVersion == currentVersion && stampedCount > 0 && stampedCount >= assetNames.size) {
                val sample = assetNames.firstOrNull()
                if (sample == null || File(pluginDir, sample).exists()) {
                    Log.d(TAG, "Plugins already extracted for version $currentVersion ($stampedCount files)")
                    needsExtraction = false
                } else {
                    Log.d(TAG, "Stamp OK but files missing on disk, re-extracting")
                }
            } else {
                Log.d(TAG, "Stamp mismatch (v=$stampedVersion/$currentVersion, n=$stampedCount/${assetNames.size}), re-extracting")
            }
        }

        if (needsExtraction) {
            Log.d(TAG, "Extracting ${assetNames.size} plugin .so files...")
            val total = assetNames.size
            var extracted = 0

            for (name in assetNames) {
                val dest = File(pluginDir, name)
                try {
                    context.assets.open("$PLUGINS_ASSET_DIR/$name").use { input ->
                        dest.outputStream().use { output ->
                            input.copyTo(output)
                        }
                    }
                    extracted++
                    onProgress?.invoke(extracted, total)
                } catch (e: Exception) {
                    Log.e(TAG, "Failed to extract $name: ${e.message}")
                }
            }

            stampFile.writeText("$currentVersion:$extracted")
            Log.d(TAG, "Extraction complete: $extracted/$total plugins")
        }

        // Pre-load DSP .so via System.load() to register them in the
        // ClassLoader's linker namespace.  Native dlopen() from filesDir
        // fails on Android 10+ (namespace restriction), but a library
        // already loaded via System.load() is found by subsequent dlopen().
        // Skip UI libs (*_ui.so) — they depend on X11/Mesa which aren't
        // loaded yet; they're handled separately by LV2PluginUI.
        preloadDspPlugins(pluginDir, assetNames)

        return true
    }

    private fun preloadDspPlugins(pluginDir: File, assetNames: List<String>) {
        var preloaded = 0
        for (name in assetNames) {
            if (name.contains("_ui.so")) continue
            val dest = File(pluginDir, name)
            if (dest.exists() && dest.length() > 0) {
                try {
                    System.load(dest.absolutePath)
                    preloaded++
                } catch (e: UnsatisfiedLinkError) {
                    Log.w(TAG, "Pre-load skip $name: ${e.message}")
                }
            }
        }
        Log.i(TAG, "Pre-loaded $preloaded DSP plugins via System.load()")
    }
}
