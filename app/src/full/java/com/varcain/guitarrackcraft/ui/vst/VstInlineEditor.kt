/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.varcain.guitarrackcraft.ui.vst

import android.util.Log
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.varcain.guitarrackcraft.engine.NativeEngine
import com.varcain.vsthost.NativeBridge
import com.varcain.vsthost.ui.PluginSurface
import kotlinx.coroutines.delay

/**
 * Embed a wine-rendered VST editor inline in the rack row. The displayNumber
 * is owned by the WineVstPlugin (vsthost_lib) — NOT GuitarRackCraft's
 * X11DisplayManager — so we go directly through vsthost_lib's NativeBridge
 * for surface attach and touch routing.
 *
 * Sizing: poll the plugin's editor_width/height via shm (via the
 * nativeGetRackPluginEditorSize JNI bridge). Until known, show "Loading
 * editor…". Once known, render vstpoc's PluginSurface which uses
 * Modifier.aspectRatio for letterboxing.
 */
@Composable
fun VstInlineEditor(
    pluginIndex: Int,
    isFullscreen: Boolean = false,
    onPluginSizeKnown: (width: Int, height: Int) -> Unit = { _, _ -> },
) {
    val displayNumber = remember(pluginIndex) {
        runCatching {
            NativeEngine.getInstance().nativeGetRackPluginX11Display(pluginIndex)
        }.getOrDefault(-1)
    }
    if (displayNumber < 0) {
        Box(modifier = Modifier.fillMaxWidth().height(60.dp).background(Color(0xFF222222)),
            contentAlignment = Alignment.Center) {
            Text("VST display unavailable", color = Color.White)
        }
        return
    }

    // Bring up the X11 server on this display if it's not already (idempotent).
    // Wine started it at activate time, but this is harmless to repeat.
    LaunchedEffect(displayNumber) {
        NativeBridge.nativeStartX11Server(displayNumber, 4096, 2160)
    }

    var size by remember(pluginIndex) { mutableStateOf<Pair<Int, Int>?>(null) }
    LaunchedEffect(pluginIndex) {
        while (size == null) {
            val encoded = runCatching {
                NativeEngine.getInstance().nativeGetRackPluginEditorSize(pluginIndex)
            }.getOrDefault(0L)
            val w = (encoded ushr 32).toInt()
            val h = (encoded and 0xffffffffL).toInt()
            if (w > 0 && h > 0) {
                Log.i("VstInlineEditor", "plugin[$pluginIndex] editor size: ${w}x$h → display=$displayNumber")
                NativeBridge.nativeSetX11PluginSize(displayNumber, w, h)
                size = w to h
                onPluginSizeKnown(w, h)
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
            modifier      = if (isFullscreen) Modifier.fillMaxSize() else Modifier.fillMaxWidth(),
            // Keep the X11 display alive across activity transitions
            // (file picker, app backgrounding, etc.). The display is
            // owned by the wine subprocess that WineVstPlugin spawned
            // — destroying it on Compose dispose orphans wine and the
            // next attach attempt crashes (SIGABRT on the main thread,
            // verified 2026-05-26 with WAV picker round-trip).
            destroyOnDispose = false,
        )
    } else {
        Box(modifier = Modifier.fillMaxWidth().height(60.dp).background(Color(0xFF111111)).padding(8.dp),
            contentAlignment = Alignment.Center) {
            Text("Loading editor…", color = Color.LightGray)
        }
    }
}
