/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.varcain.guitarrackcraft.debug

import android.app.Activity
import android.content.Intent
import android.util.Log
import com.varcain.guitarrackcraft.X11PluginUIActivity
import com.varcain.guitarrackcraft.engine.RackManager
import com.varcain.vsthost.NativeBridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * Marker-file dev hooks run once the audio engine is up. fullDebug builds only:
 * fullRelease and playstore ship no-op stubs, so release builds carry neither
 * the hooks nor the :vsthost_lib reference (which playstore can't resolve).
 */
object DebugHooks {
    suspend fun onEngineReady(activity: Activity) {
        maybeRunAhbSpike(activity)
        maybeAutostartEditor(activity)
    }

    /** Phase 0 GPU-upgrade spike (throwaway). If cache/ahbspike exists, run the
     *  cross-driver AHardwareBuffer + fence interop test in-process (the app has
     *  GPU access + adrenotools works here, same as the wine subprocess) and log
     *  PASS/FAIL to logcat tag "AhbSpike". Drop the marker with:
     *    adb shell run-as <pkg> sh -c 'printf x > cache/ahbspike'
     */
    private suspend fun maybeRunAhbSpike(activity: Activity) {
        try {
            if (!File(activity.cacheDir, "ahbspike").exists()) return
            val turnipDir = File(activity.filesDir, "wine/turnip").absolutePath + "/"  // trailing slash required
            val logPath = File(activity.cacheDir, "ahbspike.log").absolutePath
            val nativeLibDir = activity.applicationInfo.nativeLibraryDir
            withContext(Dispatchers.IO) {
                Log.i("AhbSpike", "running spike (hook=$nativeLibDir turnip=$turnipDir log=$logPath)")
                val ok = NativeBridge.nativeAhbSpike(
                    nativeLibDir, turnipDir, "vulkan.ad07xx.so", logPath)
                Log.i("AhbSpike", "spike returned ok=$ok")
            }
        } catch (e: Throwable) {
            Log.e("AhbSpike", "spike threw", e)
        }
    }

    /**
     * Debug autostart: if cache/autostart_editor.txt contains a plugin name (or
     * id substring), open that plugin's X11 editor on launch — a faithful BIAS
     * FX 2 black-editor repro with no manual rack interaction, so the FEX stack
     * recursion can be iterated on without anyone driving the device. Marker
     * absent (the normal case) = no-op. Drop the marker with:
     *   adb shell run-as <pkg> sh -c 'echo "BIAS FX 2" > cache/autostart_editor.txt'
     */
    private suspend fun maybeAutostartEditor(activity: Activity) {
        try {
            val marker = File(activity.cacheDir, "autostart_editor.txt")
            if (!marker.exists()) return
            val want = marker.readText().trim()
            if (want.isEmpty()) return
            val rm = RackManager
            val match = withContext(Dispatchers.IO) {
                rm.getAvailablePlugins().firstOrNull {
                    it.name.contains(want, ignoreCase = true) ||
                    it.fullId.contains(want, ignoreCase = true) ||
                    it.id.contains(want, ignoreCase = true)
                }
            }
            if (match == null) {
                Log.w("Autostart", "no plugin matches '$want'")
                return
            }
            // Re-add to the MAIN rack (audio chain) so it behaves like a restored
            // session, not just the editor window. Guard on empty rack so config
            // changes / re-creates don't stack duplicates. Heavy (loads plugin) -> IO.
            // Debug toggle: cache/autostart_norack present => editor only (clean,
            // single vst_host log for pipe tracing). Absent (normal) => add to rack.
            if (!File(activity.cacheDir, "autostart_norack").exists()) {
                withContext(Dispatchers.IO) {
                    if (rm.getRackSize() == 0) {
                        val pos = rm.addPlugin(match.fullId)
                        Log.i("Autostart", "added ${match.name} to rack at pos=$pos")
                    } else {
                        Log.i("Autostart", "rack already has ${rm.getRackSize()} plugin(s), skip add")
                    }
                }
            }
            // Also open its X11 editor (the BIAS FX 2 black-editor repro).
            Log.i("Autostart", "opening editor for ${match.name} (${match.fullId})")
            activity.startActivity(
                Intent(activity, X11PluginUIActivity::class.java)
                    .putExtra(X11PluginUIActivity.EXTRA_PLUGIN_ID, match.fullId)
            )
        } catch (e: Exception) {
            Log.e("Autostart", "failed", e)
        }
    }
}
