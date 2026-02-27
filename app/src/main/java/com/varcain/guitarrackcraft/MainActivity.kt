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

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.lifecycleScope
import com.varcain.guitarrackcraft.engine.AudioEngine
import com.varcain.guitarrackcraft.engine.EngineInitHelper
import com.varcain.guitarrackcraft.ui.loading.PluginExtractScreen
import com.varcain.guitarrackcraft.ui.navigation.AppNavigation
import com.varcain.guitarrackcraft.ui.theme.GuitarRackCraftTheme
import com.varcain.guitarrackcraft.ui.tone3000.Tone3000CallbackHandler
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileNotFoundException
import java.io.FileOutputStream
import java.io.InputStream

class MainActivity : ComponentActivity() {

    private var engineReady by mutableStateOf(false)
    private var extractedCount by mutableIntStateOf(0)
    private var extractTotalCount by mutableIntStateOf(0)

    override fun onNewIntent(intent: android.content.Intent?) {
        super.onNewIntent(intent)
        handleAuthIntent(intent)
    }

    private fun handleAuthIntent(intent: android.content.Intent?) {
        intent?.data?.let { uri ->
            if (uri.scheme == "guitarrackcraft") {
                when (uri.host) {
                    "tone3000auth" -> {
                        val apiKey = uri.getQueryParameter("api_key")
                        if (apiKey != null) {
                            android.util.Log.d("MainActivity", "Received Tone3000 api_key")
                            Tone3000CallbackHandler.onApiKeyReceived(apiKey)
                        }
                    }
                    "tone3000select" -> {
                        val toneUrl = uri.getQueryParameter("tone_url")
                        if (toneUrl != null) {
                            android.util.Log.d("MainActivity", "Received Tone3000 tone_url")
                            Tone3000CallbackHandler.onToneUrlReceived(toneUrl)
                        }
                    }
                }
            }
        }
    }

    private val requestPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        android.util.Log.d("MainActivity", "RECORD_AUDIO permission result: granted=$granted")
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)

        // Handle auth callback if activity started via deep link
        handleAuthIntent(intent)

        // Full-screen immersive: hide system bars, draw behind cutout
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val controller = WindowInsetsControllerCompat(window, window.decorView)
        controller.hide(WindowInsetsCompat.Type.systemBars())
        controller.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE

        // Initialize engine asynchronously. For playstore flavor, plugin .so extraction
        // may take 15-30s on first launch; show a progress screen in the meantime.
        lifecycleScope.launch {
            withContext(Dispatchers.IO) {
                prepareLv2AndInitEngine()
            }
            engineReady = true
        }

        setContent {
            GuitarRackCraftTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    if (engineReady) {
                        AppNavigation()
                    } else {
                        PluginExtractScreen(
                            extracted = extractedCount.takeIf { extractTotalCount > 0 },
                            total = extractTotalCount.takeIf { extractTotalCount > 0 }
                        )
                    }
                }
            }
        }
    }

    private fun extractLV2Assets() {
        // Extract LV2 plugins from assets to internal storage (use applicationContext for consistency with native path)
        val lv2Dir = File(applicationContext.filesDir, "lv2")
        lv2Dir.mkdirs()

        try {
            // Primary: use build-time file manifest for reliable extraction.
            // assets.list() is unreliable across split APKs on some Android versions,
            // causing entire bundles to be extracted as empty directories.
            val fileList = try {
                assets.open("lv2_files.txt").bufferedReader().use { reader ->
                    reader.readLines().map { it.trim() }.filter { it.isNotEmpty() && !it.endsWith(".so") }
                }
            } catch (_: Exception) { null }

            if (fileList != null) {
                android.util.Log.d("MainActivity", "Extracting ${fileList.size} LV2 asset files from lv2_files.txt")
                for (relPath in fileList) {
                    val outputFile = File(lv2Dir, relPath)
                    outputFile.parentFile?.mkdirs()
                    try {
                        assets.open("lv2/$relPath").use { input ->
                            FileOutputStream(outputFile).use { output ->
                                input.copyTo(output)
                            }
                        }
                    } catch (e: Exception) {
                        android.util.Log.w("MainActivity", "Failed to extract lv2/$relPath: ${e.message}")
                    }
                }
            } else {
                // Fallback: enumerate via assets.list() (full variant / no manifest)
                val bundleNames = assets.list("lv2")?.toList() ?: emptyList()
                android.util.Log.d("MainActivity", "lv2_files.txt not found, assets.list returned ${bundleNames.size} entries")
                bundleNames.forEach { assetName ->
                    val assetPath = "lv2/$assetName"
                    val outputDir = File(lv2Dir, assetName)
                    outputDir.mkdirs()
                    copyAssetRecursive(assetPath, outputDir)
                }
            }
            val bundleCount = lv2Dir.list()?.size ?: 0
            android.util.Log.d("MainActivity", "LV2 assets extracted to ${lv2Dir.absolutePath} ($bundleCount top-level entries)")
        } catch (e: Exception) {
            android.util.Log.e("MainActivity", "Failed to extract LV2 assets", e)
        }
    }
    
    private fun copyAssetRecursive(assetPath: String, outputDir: File) {
        try {
            outputDir.mkdirs()
            
            // Try to list as directory first
            val assetList = assets.list(assetPath)
            if (assetList != null && assetList.isNotEmpty()) {
                // It's a directory - recurse into it
                assetList.forEach { item ->
                    val newAssetPath = "$assetPath/$item"
                    val newOutputDir = File(outputDir, item)
                    copyAssetRecursive(newAssetPath, newOutputDir)
                }
            } else {
                // list() returned null or empty - try to open as file
                try {
                    val inputStream: InputStream = assets.open(assetPath)
                    val fileName = assetPath.substringAfterLast('/')

                    // Skip .so files — plugin binaries are loaded from nativeLibDir
                    if (fileName.endsWith(".so")) {
                        inputStream.close()
                        return
                    }
                    val outputFile = File(outputDir.parentFile ?: outputDir, fileName)
                    
                    // Remove existing file/directory if it exists
                    if (outputFile.exists()) {
                        if (outputFile.isDirectory) {
                            outputFile.deleteRecursively()
                        } else {
                            outputFile.delete()
                        }
                    }
                    
                    outputFile.parentFile?.mkdirs()
                    FileOutputStream(outputFile).use { output ->
                        inputStream.copyTo(output)
                    }
                    inputStream.close()
                } catch (e: FileNotFoundException) {
                    // Asset might not exist, skip
                    android.util.Log.d("MainActivity", "Asset not found as file: $assetPath")
                }
            }
        } catch (e: Exception) {
            android.util.Log.e("MainActivity", "Failed to copy asset: $assetPath", e)
        }
    }

    private fun prepareLv2AndInitEngine() {
        EngineInitHelper.preloadLilv(applicationInfo.nativeLibraryDir)
        extractLV2Assets()
        EngineInitHelper.initEngine(this) { extracted, total ->
            extractedCount = extracted
            extractTotalCount = total
        }
    }

    override fun onResume() {
        super.onResume()
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            android.util.Log.d("MainActivity", "RECORD_AUDIO not granted, requesting...")
            requestPermissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
        }
    }

    override fun onPause() {
        super.onPause()
        android.util.Log.i("AudioLifecycle", "MainActivity.onPause (isFinishing=$isFinishing)")
    }

    override fun onStop() {
        super.onStop()
        android.util.Log.i("AudioLifecycle", "MainActivity.onStop (isFinishing=$isFinishing)")
    }

    override fun onDestroy() {
        super.onDestroy()
        // Only stop the engine when the activity is really finishing (user left the app).
        // When isFinishing() is false, the activity is being recreated (e.g. config change);
        // don't stop the engine so the new instance keeps the same running stream.
        android.util.Log.i("AudioLifecycle", "MainActivity.onDestroy (isFinishing=$isFinishing)")
        if (isFinishing()) {
            android.util.Log.i("AudioLifecycle", "MainActivity.onDestroy (finishing) -> stopEngine()")
            AudioEngine.stop()
        }
    }
}
