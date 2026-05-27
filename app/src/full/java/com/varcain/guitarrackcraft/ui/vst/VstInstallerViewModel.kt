/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.varcain.guitarrackcraft.ui.vst

import android.app.Application
import android.content.Context
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.varcain.guitarrackcraft.engine.NativeEngine
import com.varcain.vsthost.NativeBridge
import com.varcain.vsthost.PeFlag
import com.varcain.vsthost.wine.WineSetup
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.UUID

/**
 * Drives the "Install from .exe" flow. Mirrors vstpoc's HostViewModel.runInstaller
 * state machine but scoped to GuitarRackCraft's vsthost_lib integration:
 *
 *   IDLE → PREPARING (clone wineprefix)
 *        → RUNNING (wine spawns the .exe; user clicks through the wizard)
 *        → DRAINING (~2s settle after wine exits)
 *        → DISCOVERING (walk the prefix for new VST2/VST3 DLLs)
 *        → PICK (user checks which discovered plugins to import)
 *        → IDLE (copy picks into per-plugin dirs + registry.json; cleanup template)
 *
 * The installer wizard renders on X11 [INSTALLER_DISPLAY_NUMBER] (99) — the
 * VstInstallerScreen mounts an EditorSurfaceView on that display while
 * RUNNING. Audio chain must be stopped first because we use big-core
 * affinity that the audio thread also wants.
 */
class VstInstallerViewModel(app: Application) : AndroidViewModel(app) {

    enum class State { IDLE, PREPARING, RUNNING, DRAINING, DISCOVERING, PICK }

    data class Session(
        val id: String,
        val displayName: String,
        val stagedExePath: String,
        val templatePrefixPath: String,
        val displayNumber: Int,
        val winePid: Int = -1,
    )

    data class DiscoveredPlugin(
        val absPath: String,
        val relToPrefix: String,
        val displayName: String,
        val isVst3: Boolean,
        val is64Bit: Boolean,
    )

    private val _state = MutableStateFlow(State.IDLE)
    val state: StateFlow<State> = _state.asStateFlow()

    private val _session = MutableStateFlow<Session?>(null)
    val session: StateFlow<Session?> = _session.asStateFlow()

    private val _discovered = MutableStateFlow<List<DiscoveredPlugin>>(emptyList())
    val discovered: StateFlow<List<DiscoveredPlugin>> = _discovered.asStateFlow()

    private val _errorMessage = MutableStateFlow<String?>(null)
    val errorMessage: StateFlow<String?> = _errorMessage.asStateFlow()

    private var watchJob: Job? = null

    fun consumeError() { _errorMessage.value = null }

    /** Kick off a new install. [stagedExePath] must be a file already copied
     *  into app storage (the picker URI can't be passed across processes). */
    fun installFromExe(stagedExePath: String, displayName: String) {
        if (_state.value != State.IDLE) {
            Log.w(TAG, "installFromExe called while state=${_state.value}; ignoring")
            return
        }
        val ctx = getApplication<Application>()
        val installerId = UUID.randomUUID().toString().take(8)
        val templatePath = "${ctx.filesDir.absolutePath}/wineprefix_installer_$installerId"
        _session.value = Session(
            id = installerId,
            displayName = displayName,
            stagedExePath = stagedExePath,
            templatePrefixPath = templatePath,
            displayNumber = INSTALLER_DISPLAY_NUMBER,
        )
        _state.value = State.PREPARING
        Log.i(TAG, "install: starting '$displayName' id=$installerId template=$templatePath")

        watchJob = viewModelScope.launch {
            val cloneOk = withContext(Dispatchers.IO) { prepareTemplate(ctx, templatePath) }
            if (!cloneOk) {
                bailOut("Installer setup failed: couldn't clone wineprefix.")
                return@launch
            }
            val setup = WineSetup.ensure(ctx)
            // CRITICAL: bring up the X server on display 99 BEFORE forking
            // wine. Wine's winex11.drv calls XOpenDisplay at startup; if the
            // TCP port isn't listening yet it disables X11 rendering for
            // the lifetime of the subprocess. We use the same screen-sized
            // virtual root the RunningOverlay composable expects so wine's
            // default window-positioning lands the wizard in view (otherwise
            // a 4K virtual root pushes the centred wizard offscreen).
            withContext(Dispatchers.IO) {
                NativeBridge.nativeStartX11Server(INSTALLER_DISPLAY_NUMBER,
                                                 INSTALLER_SCREEN_W, INSTALLER_SCREEN_H)
                NativeBridge.nativeSetX11PluginSize(INSTALLER_DISPLAY_NUMBER,
                                                   INSTALLER_SCREEN_W, INSTALLER_SCREEN_H)
                // Freeze the framebuffer at 1920x1080. Otherwise wine's
                // wizard CreateWindow (typically 500x350) triggers slot
                // promotion / claim-slot in X11NativeDisplay, shrinks the
                // framebuffer to wizard size, and the wizard renders into
                // a tiny letterboxed region the user can barely see.
                NativeBridge.nativeSetX11FramebufferFrozen(INSTALLER_DISPLAY_NUMBER, true)
            }
            val pid = withContext(Dispatchers.IO) {
                NativeBridge.nativeStartInstaller(
                    exePath = stagedExePath,
                    prefixPath = templatePath,
                    displayNumber = INSTALLER_DISPLAY_NUMBER,
                    wineBinary = setup.wineBinary.absolutePath,
                    wineserverBinary = setup.wineServer.absolutePath,
                    wineDllPath = setup.wineDllPath.absolutePath,
                    nativeLibDir = setup.nativeLibraryDir.absolutePath,
                    cacheDir = ctx.cacheDir.absolutePath,
                )
            }
            if (pid <= 0) {
                bailOut("Failed to launch installer (wine returned $pid). Check vst_host_installer.log.")
                return@launch
            }
            _session.value = _session.value?.copy(winePid = pid)
            _state.value = State.RUNNING
            Log.i(TAG, "install: wine pid=$pid running '$stagedExePath'")

            // Poll until the installer exits.
            while (true) {
                val r = withContext(Dispatchers.IO) { NativeBridge.nativeWaitInstaller(pid) }
                if (r == -2) {
                    delay(500)
                    continue
                }
                Log.i(TAG, "install: wine exited code=$r")
                break
            }

            _state.value = State.DRAINING
            delay(2_000)  // let wineserver write its last registry entries

            _state.value = State.DISCOVERING
            val found = withContext(Dispatchers.IO) { discoverPlugins(templatePath) }
            _discovered.value = found
            if (found.isEmpty()) {
                bailOut("Installer finished but no VST DLLs were found in the prefix.")
                return@launch
            }
            _state.value = State.PICK
            Log.i(TAG, "install: ${found.size} plugin candidate(s) — awaiting user picks")
        }
    }

    /** Confirm the user's picks: for each pick, clone the INSTALLER TEMPLATE
     *  to a per-plugin wineprefix so all support files (IRs, presets,
     *  license keys, registry entries) the installer wrote travel with the
     *  plugin. The plugin's dllPath points INSIDE that prefix at the same
     *  relative location the installer chose — so when the runtime spawns
     *  wine against this prefix, the plugin finds its own files at the
     *  exact paths it baked into its binary.
     *
     *  Trade-off: each pick gets its own full copy of the template (~500 MB
     *  for TH-U). For multi-plugin suites where many plugins share data
     *  (Native Instruments, IK Multimedia) this duplicates the dataset N
     *  times. A future optimisation could symlink shared subtrees; for now
     *  simplicity wins. */
    fun confirmPicks(picks: List<DiscoveredPlugin>) {
        val ctx = getApplication<Application>()
        val current = _session.value ?: return
        viewModelScope.launch {
            withContext(Dispatchers.IO) {
                val existing = VstRegistry.read(ctx).toMutableList()
                val templateDir = File(current.templatePrefixPath)
                for (p in picks) {
                    val uuid = UUID.randomUUID().toString()
                    val prefixDir = File(ctx.filesDir, "wineprefix_v$uuid")
                    if (prefixDir.exists()) prefixDir.deleteRecursively()
                    // Symlink-preserving clone — drive_c contents + dosdevices
                    // symlinks survive (`c:` → ../drive_c, `z:` → /). Without
                    // this both symlinks would dereference and break wine's
                    // path resolution.
                    VstHostSetup.copyPrefix(templateDir, prefixDir)
                    // Apply the same idempotent seeds ensurePluginPrefix would —
                    // DXVK, Common Controls SxS, Win7 spoof, etc. — so the
                    // cloned prefix has the same runtime tweaks a fresh
                    // per-plugin prefix gets via the standard import flow.
                    VstHostSetup.applyPluginPrefixSeeds(ctx, prefixDir)
                    // Map the plugin's path from template prefix → per-plugin
                    // prefix at the same relative location. p.relToPrefix is
                    // already "<templatePrefix-relative>/drive_c/.../foo.dll".
                    val pluginInPrefix = File(prefixDir, p.relToPrefix)
                    if (!pluginInPrefix.exists()) {
                        Log.w(TAG, "confirmPicks: plugin missing after clone " +
                                   "(uuid=$uuid, rel=${p.relToPrefix})")
                        prefixDir.deleteRecursively()
                        continue
                    }
                    existing += VstRegistryEntry(
                        uuid = uuid,
                        displayName = p.displayName,
                        format = if (p.isVst3) "VST3" else "VST2",
                        dllPath = pluginInPrefix.absolutePath,
                        is64Bit = p.is64Bit,
                    )
                    Log.i(TAG, "confirmPicks: registered $uuid (${p.displayName}) " +
                               "at $pluginInPrefix")
                }
                VstRegistry.write(ctx, existing)
                // Template now redundant — each pick has its own clone.
                templateDir.deleteRecursively()
            }
            runCatching { NativeEngine.getInstance().nativeRefreshPluginRegistry() }
            reset()
        }
    }

    /** Cancel: SIGTERM the installer, wipe template, reset state. */
    fun cancel() {
        val s = _session.value ?: return
        Log.i(TAG, "install: cancelling pid=${s.winePid}")
        watchJob?.cancel()
        if (s.winePid > 0) {
            viewModelScope.launch(Dispatchers.IO) {
                NativeBridge.nativeKillInstaller(s.winePid)
            }
        }
        viewModelScope.launch(Dispatchers.IO) {
            File(s.templatePrefixPath).deleteRecursively()
        }
        reset()
    }

    private fun bailOut(reason: String) {
        Log.e(TAG, "install: $reason")
        _errorMessage.value = reason
        val templatePath = _session.value?.templatePrefixPath
        viewModelScope.launch(Dispatchers.IO) {
            if (templatePath != null) File(templatePath).deleteRecursively()
        }
        reset()
    }

    private fun reset() {
        // Unfreeze so a future install starts from a clean placeholder.
        runCatching {
            NativeBridge.nativeSetX11FramebufferFrozen(INSTALLER_DISPLAY_NUMBER, false)
        }
        _state.value = State.IDLE
        _session.value = null
        _discovered.value = emptyList()
        watchJob = null
    }

    /** Clone the base wineprefix and apply the installer-friendly seeds:
     *  Common Controls SxS manifests (modern InstallShield/NSIS wizards
     *  need them), Program Files dirs (default InstallDir templates), and
     *  disable winemenubuilder (the "Create shortcuts" step otherwise
     *  hangs trying to write to /data/.local). */
    private fun prepareTemplate(ctx: Context, templatePath: String): Boolean {
        val basePrefix = File(ctx.filesDir, "wineprefix")
        if (!basePrefix.exists()) {
            Log.e(TAG, "prepareTemplate: base wineprefix missing at $basePrefix")
            return false
        }
        val dst = File(templatePath)
        if (dst.exists()) dst.deleteRecursively()
        return runCatching {
            VstHostSetup.copyPrefix(basePrefix, dst)
            WineSetup.seedCommonControlsManifests(dst)
            WineSetup.seedProgramFilesDirs(dst)
            WineSetup.seedDisableMenubuilder(dst)
            true
        }.getOrElse {
            Log.e(TAG, "prepareTemplate failed", it)
            false
        }
    }

    /** Walk the template prefix's drive_c/ for VST candidate files. Filters
     *  to PE files exporting VSTPluginMain or GetPluginFactory. Skips wine
     *  builtins in system32/syswow64 (cheap early bail). */
    private fun discoverPlugins(templatePath: String): List<DiscoveredPlugin> {
        val driveC = File(templatePath, "drive_c")
        if (!driveC.exists()) return emptyList()
        val out = mutableListOf<DiscoveredPlugin>()
        driveC.walkTopDown().forEach { f ->
            if (!f.isFile) return@forEach
            val lowerName = f.name.lowercase()
            if (!lowerName.endsWith(".dll") && !lowerName.endsWith(".vst3")) return@forEach
            val rel = f.relativeTo(File(templatePath)).path
            if (rel.contains("/system32/") || rel.contains("/syswow64/")) return@forEach
            val flags = runCatching {
                NativeBridge.nativeInspectPluginExports(f.absolutePath)
            }.getOrDefault(0)
            val isValidPe = (flags and PeFlag.VALID) != 0 && (flags and PeFlag.IS_DLL) != 0
            val isVst2 = (flags and PeFlag.HAS_VSTPLUGINMAIN) != 0
            val isVst3 = (flags and PeFlag.HAS_VST3_FACTORY) != 0
            if (!isValidPe || (!isVst2 && !isVst3)) return@forEach
            out += DiscoveredPlugin(
                absPath = f.absolutePath,
                relToPrefix = rel,
                displayName = f.nameWithoutExtension,
                isVst3 = isVst3,
                is64Bit = (flags and PeFlag.IS_64) != 0,
            )
        }
        Log.i(TAG, "discover[$templatePath]: ${out.size} candidates")
        return out
    }

    companion object {
        private const val TAG = "VstInstaller"
        /** Reserved X11 display slot for the installer wizard. Picked high
         *  so it never collides with per-plugin displays allocated by
         *  WineVstPlugin (which counts up from 1). */
        const val INSTALLER_DISPLAY_NUMBER = 99
        /** Virtual screen size the X server reports to wine.
         *
         *  Installer wizards have hardcoded pixel sizes baked into their .rc
         *  resources (Inno: ~503×357, InstallShield: ~600×400, NSIS modern:
         *  ~500×314). Whatever virtual screen wine sees, the wizard renders
         *  at THAT fixed pixel size and our X server's renderLoop letterboxes
         *  the whole framebuffer to fit the SurfaceView. So smaller virtual
         *  screen = bigger apparent wizard.
         *
         *  640×480 is tight against typical wizard sizes:
         *    - A 503×357 wizard fills ~78% width / ~74% height of the framebuffer
         *    - Wine still has room to center it (offset ~68, 61) — not against
         *      the edge but close enough that letterbox loss is small
         *    - On a 1080p phone landscape SurfaceView (~2200×1000), letterbox
         *      scale ~2.08× makes the wizard appear ~1045×742 — large and
         *      easily readable */
        const val INSTALLER_SCREEN_W = 640
        const val INSTALLER_SCREEN_H = 480
    }
}
