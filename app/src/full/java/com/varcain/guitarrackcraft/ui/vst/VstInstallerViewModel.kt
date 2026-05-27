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
 * Drives both the "Install from .exe" flow and the "Launch executable"
 * flow (relaunching a previously-registered manager .exe to install more
 * plugins). The two share the wineprefix + X11 + PICK plumbing — only the
 * front of the state machine differs.
 *
 *   INSTALL mode:
 *     IDLE → PREPARING (clone wineprefix from base)
 *          → RUNNING (wine spawns the .exe; user clicks through the wizard)
 *          → DRAINING → DISCOVERING → PICK → IDLE
 *
 *   LAUNCH mode (re-run a registered manager exe against its existing prefix):
 *     IDLE → RUNNING (no PREPARING — prefix already populated)
 *          → DRAINING → DISCOVERING → PICK → IDLE
 *
 * In LAUNCH mode the manager's prefix is preserved across the run; we
 * snapshot the set of VST DLLs in the prefix BEFORE launching so that
 * after-run discovery only surfaces newly-installed plugins (everything
 * already in the snapshot was either part of the base manager install or
 * already imported in a previous LAUNCH session).
 */
class VstInstallerViewModel(app: Application) : AndroidViewModel(app) {

    enum class State { IDLE, PREPARING, RUNNING, DRAINING, DISCOVERING, PICK }

    enum class Mode { INSTALL, LAUNCH }

    /** What kind of artifact the user can pick at the end of a session.
     *  EXECUTABLEs are only ever produced in INSTALL mode (managers are
     *  detected at first install, then re-launched via LAUNCH mode where
     *  re-detecting them as new managers would just confuse the user). */
    enum class Kind { VST2, VST3, EXECUTABLE }

    data class Session(
        val mode: Mode,
        val id: String,
        val displayName: String,
        /** INSTALL: path of the staged installer file the user picked.
         *  LAUNCH:  absolute path of the registered manager .exe inside its prefix. */
        val stagedExePath: String,
        /** INSTALL: path of the one-shot template prefix (deleted after PICK).
         *  LAUNCH:  path of the registered manager's permanent prefix (preserved). */
        val templatePrefixPath: String,
        val displayNumber: Int,
        val winePid: Int = -1,
        /** LAUNCH-only: absolute paths of VST DLLs that already existed in the
         *  prefix when the manager was launched. Used to compute the "new VSTs"
         *  diff after the manager exits, so the user only sees freshly-installed
         *  plugins in the PICK list. */
        val prefixVstSnapshot: Set<String> = emptySet(),
    )

    data class DiscoveredPlugin(
        val absPath: String,
        val relToPrefix: String,
        val displayName: String,
        val kind: Kind,
        val is64Bit: Boolean,
        /** File size in bytes — informational, shown next to executable
         *  candidates so the user can tell a manager exe (~10MB+) from a
         *  helper stub (~50KB). */
        val sizeBytes: Long = 0L,
    ) {
        val isVst3: Boolean get() = kind == Kind.VST3
        val isExecutable: Boolean get() = kind == Kind.EXECUTABLE
    }

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
            mode = Mode.INSTALL,
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
            runWineSession(ctx, stagedExePath, templatePath)
        }
    }

    /** Relaunch a previously-registered manager .exe against its existing
     *  wineprefix. The manager renders in the same fullscreen overlay the
     *  installer uses; on exit the prefix is re-scanned for newly-installed
     *  VSTs (diff against pre-launch snapshot) which the user can pick from. */
    fun launchExecutable(exe: VstExecutableEntry) {
        if (_state.value != State.IDLE) {
            Log.w(TAG, "launchExecutable called while state=${_state.value}; ignoring")
            return
        }
        val ctx = getApplication<Application>()
        // Show a session immediately (spinner-ish state) so the UI flips
        // to the overlay; snapshot the prefix on IO then advance to RUNNING.
        _session.value = Session(
            mode = Mode.LAUNCH,
            id = exe.uuid,
            displayName = exe.displayName,
            stagedExePath = exe.exePath,
            templatePrefixPath = exe.prefixPath,
            displayNumber = INSTALLER_DISPLAY_NUMBER,
        )
        _state.value = State.PREPARING
        watchJob = viewModelScope.launch {
            withContext(Dispatchers.IO) {
                // CRITICAL: kill any wineserver still running on this prefix
                // BEFORE applying seeds. wineserver caches the registry in
                // memory and rewrites system.reg from its cache on shutdown
                // — that would silently undo seeds we write to disk while
                // it's running. Killing it first means the next wine launch
                // starts a fresh wineserver that reads our updated registry.
                VstHostSetup.killWineserversForPrefix(exe.prefixPath)
                // Re-apply seeds on every launch — they're idempotent (each
                // checks for a version marker before writing) and this is the
                // cheapest way to upgrade prefixes registered before a new
                // seed was added (e.g., the Session Manager\Environment seed
                // landed after the IK Product Manager + Native Access etc.
                // managers had already been registered).
                VstHostSetup.applyPluginPrefixSeeds(ctx, File(exe.prefixPath))
            }
            val snapshot = withContext(Dispatchers.IO) { snapshotPrefixVsts(exe.prefixPath) }
            _session.value = _session.value?.copy(prefixVstSnapshot = snapshot)
            Log.i(TAG, "launch: starting manager '${exe.displayName}' uuid=${exe.uuid} " +
                       "prefix=${exe.prefixPath} preexisting VSTs=${snapshot.size}")
            runWineSession(ctx, exe.exePath, exe.prefixPath)
        }
    }

    /** Common spawn + wait + drain + discover + pick flow. Called by both
     *  installFromExe (after PREPARING) and launchExecutable (no PREPARING). */
    private suspend fun runWineSession(ctx: Context, exePath: String, prefixPath: String) {
        val setup = WineSetup.ensure(ctx)
        // CRITICAL: bring up the X server on display 99 BEFORE forking
        // wine. Wine's winex11.drv calls XOpenDisplay at startup; if the
        // TCP port isn't listening yet it disables X11 rendering for
        // the lifetime of the subprocess.
        withContext(Dispatchers.IO) {
            NativeBridge.nativeStartX11Server(INSTALLER_DISPLAY_NUMBER,
                                             INSTALLER_SCREEN_W, INSTALLER_SCREEN_H)
            NativeBridge.nativeSetX11PluginSize(INSTALLER_DISPLAY_NUMBER,
                                               INSTALLER_SCREEN_W, INSTALLER_SCREEN_H)
            // Freeze the framebuffer at INSTALLER_SCREEN_W×INSTALLER_SCREEN_H —
            // otherwise wine's wizard CreateWindow (typically 500x350)
            // triggers slot promotion / claim-slot in X11NativeDisplay,
            // shrinks the framebuffer to wizard size, and the wizard
            // renders into a tiny letterboxed region.
            NativeBridge.nativeSetX11FramebufferFrozen(INSTALLER_DISPLAY_NUMBER, true)
        }
        val pid = withContext(Dispatchers.IO) {
            NativeBridge.nativeStartInstaller(
                exePath = exePath,
                prefixPath = prefixPath,
                displayNumber = INSTALLER_DISPLAY_NUMBER,
                wineBinary = setup.wineBinary.absolutePath,
                wineserverBinary = setup.wineServer.absolutePath,
                wineDllPath = setup.wineDllPath.absolutePath,
                nativeLibDir = setup.nativeLibraryDir.absolutePath,
                cacheDir = ctx.cacheDir.absolutePath,
            )
        }
        if (pid <= 0) {
            bailOut("Failed to launch wine (returned $pid). Check vst_host_installer.log.")
            return
        }
        _session.value = _session.value?.copy(winePid = pid)
        _state.value = State.RUNNING
        Log.i(TAG, "session: wine pid=$pid running '$exePath'")

        // Poll until wine exits.
        while (true) {
            val r = withContext(Dispatchers.IO) { NativeBridge.nativeWaitInstaller(pid) }
            if (r == -2) {
                delay(500)
                continue
            }
            Log.i(TAG, "session: wine exited code=$r")
            break
        }

        _state.value = State.DRAINING
        delay(2_000)  // let wineserver write its last registry entries

        _state.value = State.DISCOVERING
        val session = _session.value ?: run { reset(); return }
        val found = withContext(Dispatchers.IO) {
            discoverItems(session.templatePrefixPath, session.mode, session.prefixVstSnapshot)
        }
        _discovered.value = found
        if (found.isEmpty()) {
            // INSTALL: nothing got installed (bail). LAUNCH: no NEW VSTs after the
            // manager run — that's fine, user just closed the manager without
            // installing anything new. Reset silently.
            if (session.mode == Mode.INSTALL) {
                bailOut("Installer finished but no VST DLLs were found in the prefix.")
            } else {
                Log.i(TAG, "launch: no new VSTs found after manager run; resetting")
                reset()
            }
            return
        }
        _state.value = State.PICK
        Log.i(TAG, "session: ${found.size} candidate(s) — awaiting user picks")
    }

    /** Confirm the user's picks.
     *
     *  For each VST pick: clone the active prefix to a per-plugin
     *  wineprefix_v<uuid> so the plugin's support files (IRs, presets, license
     *  keys, registry entries the installer wrote) travel with it.
     *
     *  For each EXECUTABLE pick (INSTALL mode only): clone the active prefix
     *  to wineprefix_e<uuid> so the manager exe has its own permanent prefix
     *  going forward — relaunching it from VstManagerScreen will operate
     *  against this prefix, NOT against a fresh template.
     *
     *  Trade-off: each pick gets its own full copy of the prefix (~500 MB
     *  for TH-U; multi-GB for Native Access). For multi-plugin suites this
     *  duplicates the dataset N times. A future optimisation could symlink
     *  shared subtrees; for now simplicity wins. */
    fun confirmPicks(picks: List<DiscoveredPlugin>) {
        val ctx = getApplication<Application>()
        val current = _session.value ?: return
        viewModelScope.launch {
            withContext(Dispatchers.IO) {
                val existingVsts = VstRegistry.read(ctx).toMutableList()
                val existingExes = VstExecutableRegistry.read(ctx).toMutableList()
                val sourcePrefix = File(current.templatePrefixPath)
                for (p in picks) {
                    when (p.kind) {
                        Kind.EXECUTABLE -> registerExecutablePick(ctx, sourcePrefix, p, existingExes)
                        else -> registerVstPick(ctx, sourcePrefix, p, existingVsts)
                    }
                }
                VstRegistry.write(ctx, existingVsts)
                VstExecutableRegistry.write(ctx, existingExes)

                // INSTALL mode: delete the one-shot template (each pick has
                // its own clone now). LAUNCH mode: preserve the manager's
                // prefix — it still belongs to the registered manager.
                if (current.mode == Mode.INSTALL) {
                    sourcePrefix.deleteRecursively()
                }
            }
            runCatching { NativeEngine.getInstance().nativeRefreshPluginRegistry() }
            reset()
        }
    }

    private fun registerVstPick(
        ctx: Context,
        sourcePrefix: File,
        p: DiscoveredPlugin,
        out: MutableList<VstRegistryEntry>,
    ) {
        val uuid = UUID.randomUUID().toString()
        val prefixDir = File(ctx.filesDir, "wineprefix_v$uuid")
        if (prefixDir.exists()) prefixDir.deleteRecursively()
        VstHostSetup.copyPrefix(sourcePrefix, prefixDir)
        VstHostSetup.applyPluginPrefixSeeds(ctx, prefixDir)
        val pluginInPrefix = File(prefixDir, p.relToPrefix)
        if (!pluginInPrefix.exists()) {
            Log.w(TAG, "registerVstPick: plugin missing after clone " +
                       "(uuid=$uuid, rel=${p.relToPrefix})")
            prefixDir.deleteRecursively()
            return
        }
        out += VstRegistryEntry(
            uuid = uuid,
            displayName = p.displayName,
            format = if (p.isVst3) "VST3" else "VST2",
            dllPath = pluginInPrefix.absolutePath,
            is64Bit = p.is64Bit,
        )
        Log.i(TAG, "confirmPicks: VST $uuid (${p.displayName}) at $pluginInPrefix")
    }

    private fun registerExecutablePick(
        ctx: Context,
        sourcePrefix: File,
        p: DiscoveredPlugin,
        out: MutableList<VstExecutableEntry>,
    ) {
        val uuid = UUID.randomUUID().toString()
        val prefixDir = File(ctx.filesDir, "wineprefix_e$uuid")
        if (prefixDir.exists()) prefixDir.deleteRecursively()
        VstHostSetup.copyPrefix(sourcePrefix, prefixDir)
        VstHostSetup.applyPluginPrefixSeeds(ctx, prefixDir)
        val exeInPrefix = File(prefixDir, p.relToPrefix)
        if (!exeInPrefix.exists()) {
            Log.w(TAG, "registerExecutablePick: exe missing after clone " +
                       "(uuid=$uuid, rel=${p.relToPrefix})")
            prefixDir.deleteRecursively()
            return
        }
        out += VstExecutableEntry(
            uuid = uuid,
            displayName = p.displayName,
            exePath = exeInPrefix.absolutePath,
            prefixPath = prefixDir.absolutePath,
        )
        Log.i(TAG, "confirmPicks: EXE $uuid (${p.displayName}) at $exeInPrefix")
    }

    /** Close the wine session — semantics depend on mode + current state.
     *
     *  - INSTALL + (PREPARING/RUNNING/DRAINING/DISCOVERING): hard cancel.
     *    SIGTERM wine, delete the template, reset.
     *  - INSTALL + PICK: "Discard all" — user reviewed candidates and chose
     *    none. Still delete the template + reset (no point keeping a clone
     *    around with no registered VSTs in it).
     *  - LAUNCH + RUNNING: graceful close (X = "close the manager"). SIGTERM
     *    wine; the watchJob's nativeWaitInstaller loop sees the exit and
     *    proceeds through DRAINING → DISCOVERING → PICK so the user can
     *    import any newly-installed VSTs. Prefix preserved.
     *  - LAUNCH + PICK: user discarded the new-VSTs list. Reset; prefix
     *    preserved. */
    fun cancel() {
        val s = _session.value ?: return
        val currentState = _state.value
        Log.i(TAG, "cancel: pid=${s.winePid} mode=${s.mode} state=$currentState")
        when {
            s.mode == Mode.LAUNCH && currentState == State.RUNNING -> {
                // Ask wine to exit; runWineSession's poll loop handles the rest.
                if (s.winePid > 0) {
                    viewModelScope.launch(Dispatchers.IO) {
                        NativeBridge.nativeKillInstaller(s.winePid)
                    }
                }
            }
            s.mode == Mode.LAUNCH -> {
                // PICK / DRAINING / DISCOVERING — wine already exited (or about
                // to). Just reset; the manager's prefix stays.
                watchJob?.cancel()
                reset()
            }
            else -> {
                // INSTALL: hard cancel regardless of state.
                watchJob?.cancel()
                if (s.winePid > 0 && currentState != State.PICK) {
                    viewModelScope.launch(Dispatchers.IO) {
                        NativeBridge.nativeKillInstaller(s.winePid)
                    }
                }
                viewModelScope.launch(Dispatchers.IO) {
                    File(s.templatePrefixPath).deleteRecursively()
                }
                reset()
            }
        }
    }

    private fun bailOut(reason: String) {
        Log.e(TAG, "session: $reason")
        _errorMessage.value = reason
        val s = _session.value
        if (s != null && s.mode == Mode.INSTALL) {
            viewModelScope.launch(Dispatchers.IO) {
                File(s.templatePrefixPath).deleteRecursively()
            }
        }
        reset()
    }

    private fun reset() {
        // Unfreeze so a future session starts from a clean placeholder.
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
            WineSetup.seedDefaultEnvironment(dst)
            true
        }.getOrElse {
            Log.e(TAG, "prepareTemplate failed", it)
            false
        }
    }

    /** Snapshot the set of VST DLL/VST3 absolute paths currently in [prefixPath].
     *  Used to compute the "newly installed since launch" diff after a LAUNCH
     *  session ends. Cheap directory walk — no PE inspection (we don't need
     *  to confirm VST-ness here, just identity for diffing). */
    private fun snapshotPrefixVsts(prefixPath: String): Set<String> {
        val driveC = File(prefixPath, "drive_c")
        if (!driveC.exists()) return emptySet()
        val out = mutableSetOf<String>()
        driveC.walkTopDown().forEach { f ->
            if (!f.isFile) return@forEach
            val n = f.name.lowercase()
            if (n.endsWith(".dll") || n.endsWith(".vst3")) {
                out += f.absolutePath
            }
        }
        return out
    }

    /** Walk the prefix's drive_c/ for VST candidate files AND (in INSTALL mode)
     *  executable candidates. Filters to PE files exporting VSTPluginMain or
     *  GetPluginFactory (for VSTs) or PE EXEs over a small size threshold (for
     *  executables). Skips wine builtins in system32/syswow64.
     *
     *  In LAUNCH mode: VSTs are filtered against [vstSnapshot] so only NEW
     *  plugins surface, and executable discovery is skipped (the manager is
     *  already registered — no need to re-detect it). */
    private fun discoverItems(
        prefixPath: String,
        mode: Mode,
        vstSnapshot: Set<String>,
    ): List<DiscoveredPlugin> {
        val driveC = File(prefixPath, "drive_c")
        if (!driveC.exists()) return emptyList()
        val out = mutableListOf<DiscoveredPlugin>()
        driveC.walkTopDown().forEach { f ->
            if (!f.isFile) return@forEach
            val lowerName = f.name.lowercase()
            val rel = f.relativeTo(File(prefixPath)).path
            if (rel.contains("/system32/") || rel.contains("/syswow64/")) return@forEach

            when {
                lowerName.endsWith(".dll") || lowerName.endsWith(".vst3") -> {
                    if (mode == Mode.LAUNCH && f.absolutePath in vstSnapshot) return@forEach
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
                        kind = if (isVst3) Kind.VST3 else Kind.VST2,
                        is64Bit = (flags and PeFlag.IS_64) != 0,
                        sizeBytes = f.length(),
                    )
                }
                lowerName.endsWith(".exe") -> {
                    if (mode == Mode.LAUNCH) return@forEach
                    if (isLikelyHelperOrUninstaller(lowerName)) return@forEach
                    if (f.length() < EXE_MIN_BYTES) return@forEach
                    val flags = runCatching {
                        NativeBridge.nativeInspectPluginExports(f.absolutePath)
                    }.getOrDefault(0)
                    val isValidPe = (flags and PeFlag.VALID) != 0
                    val isExe = isValidPe && (flags and PeFlag.IS_DLL) == 0
                    if (!isExe) return@forEach
                    out += DiscoveredPlugin(
                        absPath = f.absolutePath,
                        relToPrefix = rel,
                        displayName = f.nameWithoutExtension,
                        kind = Kind.EXECUTABLE,
                        is64Bit = (flags and PeFlag.IS_64) != 0,
                        sizeBytes = f.length(),
                    )
                }
            }
        }
        Log.i(TAG, "discover[$prefixPath, mode=$mode]: ${out.size} candidates " +
                   "(${out.count { it.kind != Kind.EXECUTABLE }} VST, " +
                   "${out.count { it.kind == Kind.EXECUTABLE }} exe)")
        return out
    }

    /** Conservative filter: drop the most obvious "definitely-not-a-manager"
     *  candidates so the user's PICK list isn't drowned in uninstaller stubs
     *  and Visual C++ redistributables. Anything not matched here still shows
     *  up — defaulting to unchecked — so the user can scroll the full list
     *  if their actual manager has an unusual name. */
    private fun isLikelyHelperOrUninstaller(lowerName: String): Boolean {
        return lowerName.startsWith("unins") ||
                lowerName.startsWith("uninstall") ||
                lowerName.startsWith("vc_redist") ||
                lowerName.startsWith("vcredist") ||
                lowerName.startsWith("dotnet") ||
                lowerName == "regsvr32.exe" ||
                lowerName == "rundll32.exe"
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

        /** Minimum size for an .exe to be considered as a manager candidate.
         *  Most legitimate managers are >1 MB (Inno installers themselves are
         *  ~500KB stubs but real manager exes contain UI assets). 200 KB is
         *  conservative — drops the smallest helper stubs without hiding
         *  modest single-file managers. */
        private const val EXE_MIN_BYTES = 200L * 1024L
    }
}
