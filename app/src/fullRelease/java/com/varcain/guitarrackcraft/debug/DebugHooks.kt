/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.varcain.guitarrackcraft.debug

import android.app.Activity

/**
 * fullRelease stub. The marker-file dev hooks (cache/ahbspike, cache/autostart_editor.txt)
 * only exist in fullDebug builds — see app/src/fullDebug/.../debug/DebugHooks.kt.
 */
object DebugHooks {
    @Suppress("UNUSED_PARAMETER")
    suspend fun onEngineReady(activity: Activity) {
        // intentionally empty — no dev hooks in fullRelease builds
    }
}
