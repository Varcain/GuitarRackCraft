/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.varcain.guitarrackcraft.ui.vst

import com.varcain.guitarrackcraft.engine.NativeEngine
import com.varcain.vsthost.ui.EditorViewRegistry

/** Resolve `pluginIndex → vsthost displayNumber` via the JNI bridge, then
 *  request the soft keyboard against that display's EditorSurfaceView. The
 *  view registered itself in EditorViewRegistry when PluginSurface
 *  composed; no-op if it isn't mounted yet (rack chrome shouldn't show the
 *  button before the inline editor renders, but be defensive). */
object VstKeyboardAction {
    fun showKeyboard(pluginIndex: Int) {
        val displayNumber = runCatching {
            NativeEngine.getInstance().nativeGetRackPluginX11Display(pluginIndex)
        }.getOrDefault(-1)
        if (displayNumber < 0) return
        EditorViewRegistry.showKeyboard(displayNumber)
    }
}
