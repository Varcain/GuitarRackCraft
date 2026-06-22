/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.varcain.guitarrackcraft

import android.app.Application
import android.util.Log
import com.varcain.guitarrackcraft.engine.NativeEngine
import com.varcain.guitarrackcraft.engine.RendererPreferenceManager
import com.varcain.guitarrackcraft.engine.WineEnvFile
import com.varcain.guitarrackcraft.ui.vst.VstHostSetup
import com.varcain.guitarrackcraft.ui.vst.VstRegistry
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

/**
 * Full-flavor Application that eagerly runs VstHostSetup.ensureWineRoot()
 * on a background thread at process start. Without this, the wine binaries
 * aren't extracted until the user taps "Import VST", which means VstFactory
 * registers with an empty registry, the wine subprocess fails to spawn, etc.
 *
 * Trade-off: cold start of the full flavor takes ~5-10s longer than playstore
 * because we're extracting symlinks and seeding wineprefix up front. Worth it
 * for the much simpler "import-and-run" flow.
 *
 * Also re-applies any per-plugin prefixes for plugins that were previously
 * imported, in case the user deleted them or the setup version bumped.
 */
class VstHostApplication : Application() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    override fun onCreate() {
        super.onCreate()
        // VstHostApplication.onCreate fires in EVERY process that this app
        // starts (main + :vstui editor process). Wine setup must only run
        // in the main process — the :vstui process attaches an X11 surface
        // to an ALREADY-RUNNING wine subprocess owned by main, and it has
        // no audio engine / PluginRegistry to feed.
        val procName = currentProcessName()
        if (procName != null && procName.contains(":")) {
            Log.i(TAG, "VstHostApplication.onCreate skipping setup in non-main process: $procName")
            return
        }
        Log.i(TAG, "VstHostApplication.onCreate — staging wine on background thread")
        scope.launch {
            val t0 = System.currentTimeMillis()
            val ok = VstHostSetup.ensureWineRoot(this@VstHostApplication)
            Log.i(TAG, "ensureWineRoot ok=$ok in ${System.currentTimeMillis() - t0} ms")
            if (!ok) return@launch
            // Sync the chosen plugin-editor renderer into cache/wine_env.txt BEFORE
            // any plugin can be activated (the wine subprocess is forked at
            // activate() and reads the file then). Defaults to Turnip on Adreno,
            // lavapipe elsewhere; preserves any other dev lines in the file.
            WineEnvFile.applyRenderer(
                this@VstHostApplication,
                RendererPreferenceManager.getRenderer(this@VstHostApplication)
            )
            // Re-seed each existing imported VST's prefix (idempotent).
            val entries = VstRegistry.read(this@VstHostApplication)
            for (e in entries) {
                VstHostSetup.ensurePluginPrefix(this@VstHostApplication, e.uuid)
            }
            // Once setup is done, refresh the plugin registry so VstFactory
            // re-reads registry.json and the imported plugins show up in
            // the browser without an engine restart.
            if (entries.isNotEmpty()) {
                runCatching { NativeEngine.getInstance().nativeRefreshPluginRegistry() }
            }
            Log.i(TAG, "VstHost background setup complete (${entries.size} VST(s))")
        }
    }

    private fun currentProcessName(): String? = try {
        // API 28+ has Application.getProcessName(); we're guaranteed 28+ on
        // full flavor (targetSdk=28) so this is safe.
        android.app.Application.getProcessName()
    } catch (t: Throwable) {
        Log.w(TAG, "getProcessName failed: $t")
        null
    }

    companion object { private const val TAG = "VstHostApplication" }
}
