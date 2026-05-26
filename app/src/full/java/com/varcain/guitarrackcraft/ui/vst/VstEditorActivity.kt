/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.varcain.guitarrackcraft.ui.vst

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import com.varcain.guitarrackcraft.engine.NativeEngine
import com.varcain.vsthost.NativeBridge
import com.varcain.vsthost.ui.PluginSurface
import kotlinx.coroutines.delay

/**
 * Hosts a single VST plugin's wine-rendered editor surface. Runs in the
 * MAIN process (NOT a separate :vstui process) because the in-process X11
 * server lives in libvsthost.so which was loaded by the audio chain; a
 * separate process can't share it.
 *
 * Uses vsthost_lib's PluginSurface Composable for aspect-correct sizing
 * and touch routing. The X server letterboxes the plugin's framebuffer
 * inside the SurfaceView so the plugin appears centered with black bars.
 */
class VstEditorActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val displayNumber = intent.getIntExtra(EXTRA_DISPLAY_NUMBER, -1)
        val rackPosition  = intent.getIntExtra(EXTRA_RACK_POSITION, -1)
        if (displayNumber < 0) {
            Log.e(TAG, "missing EXTRA_DISPLAY_NUMBER")
            finish(); return
        }
        Log.i(TAG, "onCreate display=$displayNumber rack=$rackPosition")

        // Start the X11 server early (idempotent if already up — wine subprocess
        // brought it up at activate(), but harmless to call again).
        NativeBridge.nativeStartX11Server(displayNumber, 4096, 2160)

        setContent {
            MaterialTheme {
                Surface(modifier = Modifier.fillMaxSize(), color = Color.Black) {
                    EditorScreen(displayNumber = displayNumber, rackPosition = rackPosition)
                }
            }
        }
    }

    companion object {
        private const val TAG = "VstEditorActivity"
        const val EXTRA_DISPLAY_NUMBER = "display_number"
        const val EXTRA_RACK_POSITION  = "rack_position"

        fun intent(ctx: Context, displayNumber: Int, rackPosition: Int = -1): Intent =
            Intent(ctx, VstEditorActivity::class.java).apply {
                putExtra(EXTRA_DISPLAY_NUMBER, displayNumber)
                putExtra(EXTRA_RACK_POSITION,  rackPosition)
            }
    }
}

@androidx.compose.runtime.Composable
private fun EditorScreen(displayNumber: Int, rackPosition: Int) {
    // Poll the rack-position-keyed editor size until wine populates it via
    // shm. Wine sets editor_width/height after effEditGetRect (~100-500 ms
    // post-activate). Default to a square placeholder until known.
    var size by remember { mutableStateOf<Pair<Int, Int>?>(null) }
    LaunchedEffect(rackPosition) {
        if (rackPosition < 0) return@LaunchedEffect
        while (size == null) {
            val encoded = runCatching {
                NativeEngine.getInstance().nativeGetRackPluginEditorSize(rackPosition)
            }.getOrDefault(0L)
            val w = (encoded ushr 32).toInt()
            val h = (encoded and 0xffffffffL).toInt()
            if (w > 0 && h > 0) {
                Log.i("VstEditorActivity", "editor size known: ${w}x$h")
                NativeBridge.nativeSetX11PluginSize(displayNumber, w, h)
                size = w to h
            } else {
                delay(250)
            }
        }
    }

    val s = size
    if (s != null) {
        PluginSurface(
            displayNumber = displayNumber,
            pluginWidth   = s.first,
            pluginHeight  = s.second,
            modifier      = Modifier.fillMaxSize(),
        )
    } else {
        // Loading state — plugin's editor size not yet reported by wine.
        Box(
            modifier = Modifier.fillMaxSize().background(Color.Black),
            contentAlignment = Alignment.Center,
        ) {
            Text("Loading editor…", color = Color.White)
        }
    }
}
