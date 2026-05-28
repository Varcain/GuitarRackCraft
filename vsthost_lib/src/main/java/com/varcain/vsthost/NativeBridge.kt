package com.varcain.vsthost

object NativeBridge {
    init {
        System.loadLibrary("vsthost")
    }

    // ── PE introspection ─────────────────────────────────────────────────
    /** Probe a PE file's export directory WITHOUT loading the DLL.
     *
     *  Returned bitfield (see [PeFlag]):
     *    bit 0 = parseable PE32/PE32+
     *    bit 1 = IMAGE_FILE_DLL set
     *    bit 2 = PE32+ (x86_64); else PE32 (x86)
     *    bit 3 = exports VSTPluginMain or main  (VST2)
     *    bit 4 = exports GetPluginFactory       (VST3)
     *
     *  Used during plugin import to classify candidate DLLs without firing
     *  up wine+FEX per file. Implementation: util/PeExports.cpp. */
    external fun nativeInspectPluginExports(path: String): Int

    // ── Installer subprocess ─────────────────────────────────────────────
    /** Spawn `wine <exePath>` against [prefixPath] (a wineprefix dir, usually
     *  a one-shot clone of the base prefix) with the wizard rendering to
     *  the in-process X server's slot [displayNumber]. Returns the wine
     *  child PID, or -1 on launch failure / installer already running.
     *
     *  Wire-format mirrors [nativeRunWineboot] — the caller passes paths
     *  resolved from WineSetup.ensure(). The installer's wizard windows
     *  show up in the SurfaceView bound to [displayNumber]; touch + key
     *  events flow through nativeInjectX11{Touch,Key} the same way as for
     *  plugin editors. */
    external fun nativeStartInstaller(
        exePath: String,
        prefixPath: String,
        displayNumber: Int,
        wineBinary: String,
        wineserverBinary: String,
        wineDllPath: String,
        nativeLibDir: String,
        cacheDir: String,
    ): Int

    /** Non-blocking poll for the installer started via [nativeStartInstaller].
     *    -1   no installer is registered / pid mismatch
     *    -2   still running — call again later
     *    >=0  installer exited with this code (or 128+signo if killed)
     *  Poll from a coroutine until you get something other than -2. */
    external fun nativeWaitInstaller(pid: Int): Int

    /** SIGTERM the running installer; SIGKILL after a grace period. Used
     *  by the Cancel button on VstInstallerScreen. No-op if [pid] doesn't
     *  match the registered installer. */
    external fun nativeKillInstaller(pid: Int)

    // ── Wineboot one-shot ────────────────────────────────────────────────
    /** Run `wine wineboot.exe --update` against [prefixPath] synchronously.
     *
     *  Used once per wineprefix (gated on a marker file) so wine.inf's
     *  RegisterDllsSection runs, populating HKCR\Interface\{IID}\
     *  ProxyStubClsid32 registrations. Without those, CoMarshalInterface
     *  returns E_NOINTERFACE for any non-IUnknown interface, breaking
     *  RegisterDragDrop and any plugin that depends on cross-apartment
     *  COM marshalling.
     *
     *  Returns:
     *    >= 0  wineboot exit code (0 = success)
     *    -1    launch failure (wineboot.exe missing, fork failed)
     *    -2    timeout (wineboot took longer than [timeoutSec])
     *
     *  Blocks the calling thread — call from Dispatchers.IO. */
    external fun nativeRunWineboot(
        prefixPath: String,
        wineBinary: String,
        wineserverBinary: String,
        wineDllPath: String,
        nativeLibDir: String,
        cacheDir: String,
        timeoutSec: Int,
    ): Int

    // ── In-process X11 server bindings ───────────────────────────────────
    // The server listens on TCP 127.0.0.1:(6000+displayNumber). Each
    // display owns one EGL surface and renders X11 client output to it.

    /** Boot the X11 protocol server early (no Surface needed yet) so wine
     *  clients can connect and start drawing while the UI is still waiting
     *  for the editor size to be known. [nativeAttachSurfaceToX11Display]
     *  later hooks up EGL + on-screen rendering. */
    external fun nativeStartX11Server(displayNumber: Int, placeholderW: Int, placeholderH: Int)

    external fun nativeAttachSurfaceToX11Display(
        displayNumber: Int,
        surface: android.view.Surface,
        width: Int,
        height: Int,
    ): Long

    external fun nativeDetachAndDestroyX11Display(displayNumber: Int)

    /** Tell the X server the plugin's native editor dimensions so its
     *  framebuffer is sized correctly and the renderLoop letterboxes the
     *  actual editor instead of clipping it. */
    external fun nativeSetX11PluginSize(displayNumber: Int, width: Int, height: Int)

    /** Freeze the X server's framebuffer on this display against the
     *  slot-promotion + claim-slot codepaths that would otherwise shrink
     *  it to the first wine window's bounds. Used by the installer flow:
     *  the wizard window is typically ~500x350 but we want the user to
     *  see the wine virtual desktop at 1920x1080 with the wizard centered
     *  inside. Without this freeze, the X server "promotes" the wizard
     *  and the framebuffer collapses to wizard size, hiding the desktop
     *  context (and making the wizard's own positioning look broken). */
    external fun nativeSetX11FramebufferFrozen(displayNumber: Int, frozen: Boolean)

    /** Inject a pointer event into display N's plugin window.
     *  action: 0 = ButtonPress(1),  1 = ButtonRelease(1),
     *          2 = MotionNotify,    3 = ButtonPress(3)+ButtonRelease(3)
     *  Right-click (action=3) is dispatched on long-press from PluginSurface;
     *  wine's x11drv translates X11 button 3 into WM_RBUTTONDOWN/UP. */
    external fun nativeInjectX11Touch(displayNumber: Int, action: Int, x: Int, y: Int)

    /** Inject a key event into display N's focused window.
     *  action: 0 = press, 1 = release.
     *  keycode: X11 hardware keycode (see [util.X11Keymap]).
     *  state:   X11 modifier bitmask (Shift=0x01, Control=0x04, Mod1=0x08). */
    external fun nativeInjectX11Key(displayNumber: Int, action: Int, keycode: Int, state: Int)

    // ── Health / diagnostics ─────────────────────────────────────────────
    /** Read the health fields out of a plugin's VstpocShared mmap file at
     *  [shmPath] (typically `<filesDir>/tmp/vst_shm_v<uuid>.dat`). Works
     *  live AND post-mortem — the file persists after the wine subprocess
     *  exits. Returns the raw long[] (see index layout in jni_runtime.cpp's
     *  nativeReadPluginHealth) or null if the file can't be read.
     *
     *  Prefer [getPluginHealth] which unpacks this into [PluginHealth]. */
    external fun nativeReadPluginHealth(shmPath: String): LongArray?

    /** Read + decode a plugin's health snapshot. [filesDir] is the app's
     *  filesDir absolute path, [uuid] the imported-VST uuid (registry.json
     *  key). Returns null if the plugin was never activated (no shm file)
     *  or the read failed. */
    fun getPluginHealth(filesDir: String, uuid: String): PluginHealth? {
        val shmPath = "$filesDir/tmp/vst_shm_v$uuid.dat"
        val v = nativeReadPluginHealth(shmPath) ?: return null
        if (v.size < 13) return null
        return PluginHealth(
            layoutVersion = v[0].toInt(),
            dxvkInitStatus = v[1].toInt(),
            d3d11DeviceStatus = v[2].toInt(),
            renderApiUsed = v[3].toInt(),
            lastMemAllocFailedSize = v[4],
            lastMemAllocFailedTypes = v[5].toInt(),
            lastMemAllocFailedCount = v[6],
            paintRequestCount = v[7],
            wmPaintCount = v[8],
            vehPatternsHitBitmask = v[9],
            wmUserStormPerSecond = v[10].toInt(),
            loadStatus = v[11].toInt(),
            guestReady = v[12] != 0L,
        )
    }
}

/** Decoded health snapshot of one VST plugin's wine subprocess. Field
 *  semantics mirror VstpocShared's health block in external/shared_layout.h.
 *  A [layoutVersion] of 0 means the guest pre-dates the health fields —
 *  every field below is meaningless in that case. */
data class PluginHealth(
    val layoutVersion: Int,
    val dxvkInitStatus: Int,       // 0=n/a 1=ok 2=mem_fail 3=create_fail 4=other
    val d3d11DeviceStatus: Int,    // 0=not_created 1=ok 2=failed
    val renderApiUsed: Int,        // bitmask: 1=d3d11 2=d3d9 4=gl 8=gdi 16=none
    val lastMemAllocFailedSize: Long,
    val lastMemAllocFailedTypes: Int,
    val lastMemAllocFailedCount: Long,
    val paintRequestCount: Long,
    val wmPaintCount: Long,
    val vehPatternsHitBitmask: Long,
    val wmUserStormPerSecond: Int,
    val loadStatus: Int,           // 0=pending 1=ok 2=failed (existing field)
    val guestReady: Boolean,
) {
    val isLegacyGuest: Boolean get() = layoutVersion == 0

    /** Heuristic: stuck in a JUCE event loop with no rendering (the
     *  AmpliTube/TH-U-64 black-screen signature). */
    val looksLikeBlackScreenStall: Boolean
        get() = !isLegacyGuest &&
            wmUserStormPerSecond > 100 &&
            paintRequestCount == 0L &&
            wmPaintCount == 0L

    /** Human-readable one-liner for logs / debug UI. */
    fun summarize(): String {
        if (isLegacyGuest) return "health: legacy guest (no health fields)"
        val dxvk = when (dxvkInitStatus) {
            0 -> "dxvk:n/a"; 1 -> "dxvk:ok"; 2 -> "dxvk:MEM_FAIL"
            3 -> "dxvk:CREATE_FAIL"; else -> "dxvk:err"
        }
        val d3d = when (d3d11DeviceStatus) {
            0 -> "d3d11:none"; 1 -> "d3d11:ok"; else -> "d3d11:FAIL"
        }
        val paint = if (paintRequestCount == 0L && wmUserStormPerSecond > 100)
            "paint:STALLED" else "paint:$paintRequestCount"
        return "health: $dxvk $d3d $paint veh=0x${vehPatternsHitBitmask.toString(16)} " +
            "loadStatus=$loadStatus ready=$guestReady"
    }
}

/** Bit positions returned by [NativeBridge.nativeInspectPluginExports]. */
object PeFlag {
    const val VALID = 1 shl 0
    const val IS_DLL = 1 shl 1
    const val IS_64 = 1 shl 2
    const val HAS_VSTPLUGINMAIN = 1 shl 3
    const val HAS_VST3_FACTORY = 1 shl 4

    /** True iff the file at [path] is a 64-bit VST2 plugin DLL (parseable
     *  PE32+ with the IMAGE_FILE_DLL bit AND exports VSTPluginMain/main). */
    fun isVst2Plugin(path: String): Boolean {
        val flags = NativeBridge.nativeInspectPluginExports(path)
        return (flags and VALID) != 0 &&
            (flags and IS_DLL) != 0 &&
            (flags and HAS_VSTPLUGINMAIN) != 0
    }
}
