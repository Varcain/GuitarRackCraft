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
