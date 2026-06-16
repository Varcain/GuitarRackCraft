package com.varcain.guitarrackcraft.debug

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import com.varcain.guitarrackcraft.BuildConfig
import com.varcain.vsthost.NativeBridge
import com.varcain.vsthost.wine.WineSetup

/**
 * Debug-only hook to fork `wine <exe>` against a wineprefix straight from adb,
 * with NO UI interaction — for fast iteration on plugin/service bring-up (e.g.
 * the PACE/iLok PaceLicenseDServices LDSvc.exe crash).
 *
 * The wine fork MUST come from the app process (SELinux untrusted_app domain —
 * `adb shell run-as` can't execmem), so this runs in-process via the same
 * NativeBridge.nativeStartInstaller() path the installer UI uses. Boots services
 * in the prefix first (so auto-start services like PaceLicenseDServices fire),
 * then runs the given exe. Extra args via cache/exe_args.txt; WINEDEBUG via
 * cache/wine_env.txt. Output → cache/vst_host_installer.log.
 *
 *   adb shell am broadcast -n com.varcain.guitarrackcraft/com.varcain.guitarrackcraft.debug.DebugRunReceiver \
 *     -a com.varcain.guitarrackcraft.DEBUG_RUN \
 *     --es exe "/data/.../wineprefix_X/drive_c/.../LDSvc.exe" \
 *     --es prefix "/data/.../wineprefix_X"
 */
class DebugRunReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (!BuildConfig.DEBUG) {
            Log.w(TAG, "ignored — not a debug build")
            return
        }
        val exe = intent.getStringExtra("exe")
        val prefix = intent.getStringExtra("prefix")
        if (exe.isNullOrBlank() || prefix.isNullOrBlank()) {
            Log.e(TAG, "missing extras — need --es exe <unixpath> --es prefix <wineprefix>")
            return
        }
        val display = intent.getIntExtra("display", 99)
        val appCtx = context.applicationContext
        Thread {
            try {
                val setup = WineSetup.ensure(appCtx)
                val pid = NativeBridge.nativeStartInstaller(
                    exePath = exe,
                    prefixPath = prefix,
                    displayNumber = display,
                    wineBinary = setup.wineBinary.absolutePath,
                    wineserverBinary = setup.wineServer.absolutePath,
                    wineDllPath = setup.wineDllPath.absolutePath,
                    nativeLibDir = setup.nativeLibraryDir.absolutePath,
                    cacheDir = appCtx.cacheDir.absolutePath,
                )
                Log.i(TAG, "started wine pid=$pid  exe=$exe  prefix=$prefix  (log: cache/vst_host_installer.log)")
            } catch (t: Throwable) {
                Log.e(TAG, "debug run failed", t)
            }
        }.start()
    }

    companion object {
        private const val TAG = "DebugRun"
    }
}
