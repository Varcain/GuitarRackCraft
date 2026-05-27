package com.varcain.vsthost.wine

import android.content.Context
import android.system.ErrnoException
import android.system.Os
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.util.zip.GZIPInputStream

/**
 * First-run setup for the fex-pivot wine chain.
 *
 *  Layout on device (no proot, no chroot, no box64):
 *
 *    <files_dir>/wine/                  wine install root (chroot-style paths)
 *      bin/{wine,wineserver,wine-preloader}            ELF symlinks → libwine_NNNN.so
 *      lib/wine/aarch64-unix/<name>.so                 ELF symlinks → libwine_NNNN.so
 *      lib/wine/aarch64-windows/<name>.dll             PE symlinks  → libwine_NNNN.so
 *      lib/wine/aarch64-windows/libarm64ecfex.dll      FEX symlinks → libwine_NNNN.so
 *      lib/wine/aarch64-windows/libwow64fex.dll
 *
 *    <files_dir>/wineprefix/            user prefix (wineboot populates on first run)
 *      drive_c/...
 *      dosdevices/c: -> ../drive_c
 *      dosdevices/z: -> /
 *      drive_c/windows/winsxs/manifests/...            Common-Controls 6 SxS
 *
 *  Why the symlink → libwine_NNNN.so dance: Android 10+ blocks PROT_EXEC
 *  mmaps on files under /data/data/<app>/files/. Only files in
 *  nativeLibraryDir (extracted from the APK's lib/<abi>/) get the
 *  app_executable_file SELinux label that allows exec. Wine binaries must
 *  live there but wine wants them at fixed paths like <wine_root>/bin/wine.
 *  Symlinks resolve to the actual file (in nativeLibraryDir) at execve
 *  time, so the SELinux check passes on the exec-allowed inode.
 *
 *  Master branch's WineSetup.kt was much heavier — it had to handle a proot
 *  chroot, box64's PT_INTERP paradox, and a Winlator rootfs tarball. None
 *  of that applies here: arm64 wine runs natively under Bionic.
 */
object WineSetup {
    private const val TAG = "WineSetup"

    /** Bump when the manifest schema changes so the install is rebuilt. */
    private const val SETUP_VERSION = 2

    data class Setup(
        val wineRoot: File,
        val winePrefix: File,
        val wineBinary: File,
        val wineServer: File,
        val wineDllPath: File,
        val nativeLibraryDir: File,
    )

    fun ensure(ctx: Context): Setup {
        val wineRoot = File(ctx.filesDir, "wine")
        val winePrefix = File(ctx.filesDir, "wineprefix")
        val nativeLibDir = File(ctx.applicationInfo.nativeLibraryDir)
        val versionFile = File(wineRoot, ".vstpoc-fex-version")

        // The version file stores SETUP_VERSION + nativeLibraryDir path.
        // Android randomizes the APK install path on every install, so symlinks
        // baked at extraction-time go stale. Refresh manifest symlinks every
        // launch (cheap); only rebuild the whole tree when SETUP_VERSION
        // changes.
        val expected = "$SETUP_VERSION\n${nativeLibDir.absolutePath}"
        val current = if (versionFile.exists()) versionFile.readText() else null
        val needFullRebuild = current == null ||
            current.lineSequence().firstOrNull()?.trim()?.toIntOrNull() != SETUP_VERSION

        if (needFullRebuild) {
            Log.i(TAG, "wine install full rebuild (have=$current, want=v$SETUP_VERSION)")
            wineRoot.deleteRecursively()
            wineRoot.mkdirs()
        }

        val t0 = System.currentTimeMillis()
        applyManifestSymlinks(ctx, wineRoot, nativeLibDir)
        if (needFullRebuild) {
            extractNlsTarball(ctx, wineRoot)
        }
        // DO NOT extract wine.inf. When wine finds wine.inf with a newer
        // mtime than `<prefix>/.update-timestamp`, ntdll's prefix-update
        // path runs wineboot's update_wineprefix → DefaultInstall section,
        // which calls menubuilder + winedevice + service start hooks. None
        // of those work in our Bionic environment (no ntoskrnl.exe, no /data/
        // .local writable, etc.) and the plugin's vst_host.exe gets stuck
        // waiting on them — observable as the "still loads, gives a toast"
        // hang. Keeping wine.inf out of share/wine makes the prefix-update
        // logic a no-op, which is the behavior the working baseline relied on.
        // If we ever want to populate proxy/stub registrations, we'll have
        // to seed them directly in system.reg (see task #126) rather than
        // letting wineboot do it.
        // extractWineInf(ctx, wineRoot)
        // Clean up any wine.inf that an earlier build's extractWineInf
        // dropped — without this, even after revert wine keeps auto-updating
        // on every launch.
        File(wineRoot, "share/wine/wine.inf").delete()
        seedWinePrefix(winePrefix)
        seedSystem32(wineRoot, winePrefix)
        seedUserFolders(ctx, winePrefix)
        seedFonts(ctx, winePrefix)
        seedCommonControlsManifests(winePrefix)
        seedRpcSsService(winePrefix)
        /* Also seed every existing wineprefix_template_<id> dir (installer
         * mode plugins like X50II clone from their own template, not from
         * the base prefix — without this, the RpcSs registration only
         * helps IMPORTED .dll plugins, not INSTALLED ones). */
        winePrefix.parentFile?.listFiles { f ->
            f.isDirectory && f.name.startsWith("wineprefix_template_")
        }?.forEach { template ->
            seedRpcSsService(template)
        }
        versionFile.writeText(expected)
        Log.i(TAG, "wine install ready in ${System.currentTimeMillis() - t0} ms")

        return Setup(
            wineRoot = wineRoot,
            winePrefix = winePrefix,
            wineBinary = File(wineRoot, "bin/wine"),
            wineServer = File(wineRoot, "bin/wineserver"),
            wineDllPath = File(wineRoot, "lib/wine/aarch64-windows"),
            nativeLibraryDir = nativeLibDir,
        )
    }

    /** Extract `wine-fex-nls.tar.gz` from assets into <wineRoot>/share/wine.
     *  Wine looks at `<bin_dir>/../share/wine/nls/locale.nls` and similar at
     *  startup; the WINEDATADIR env var (set by WineHostProcess and honored
     *  by our patched wine ntdll) points at <wineRoot>/share/wine so the
     *  files end up at the path wine expects. NLS files are not executable
     *  so they don't need to live in jniLibs. */
    private fun extractNlsTarball(ctx: Context, wineRoot: File) {
        val target = File(wineRoot, "share/wine")
        target.mkdirs()
        // AAPT2 unwraps `.tar.gz` to `.tar` at build time (the APK zip will
        // re-deflate, no point double-compressing). Fall through to the
        // gzipped name if AAPT didn't unwrap (e.g. noCompress is set later).
        val (assetName, gzipped) = when {
            assetExists(ctx, "wine-fex-nls.tar") -> "wine-fex-nls.tar" to false
            assetExists(ctx, "wine-fex-nls.tar.gz") -> "wine-fex-nls.tar.gz" to true
            else -> {
                Log.w(TAG, "wine-fex-nls.tar(.gz) missing from assets")
                return
            }
        }
        try {
            ctx.assets.open(assetName).use { rawIn ->
                val src: InputStream = if (gzipped) GZIPInputStream(rawIn) else rawIn
                src.use { stream ->
                    val tar = TarReader(stream)
                    var nFiles = 0
                    while (true) {
                        val entry = tar.nextEntry() ?: break
                        if (entry.name.isEmpty()) continue
                        // Tarball is rooted at "nls/..."; we extract under share/wine/.
                        val dst = File(target, entry.name)
                        when (entry.type) {
                            TarEntry.Type.DIR -> dst.mkdirs()
                            TarEntry.Type.FILE -> {
                                dst.parentFile?.mkdirs()
                                FileOutputStream(dst).use { fos -> tar.copyEntryTo(fos) }
                                nFiles++
                            }
                            else -> { /* skip symlinks etc */ }
                        }
                    }
                    Log.i(TAG, "extracted $nFiles NLS files to ${target.absolutePath}")
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "failed to extract $assetName: ${e.message}")
        }
    }

    private fun assetExists(ctx: Context, name: String): Boolean = try {
        ctx.assets.open(name).close(); true
    } catch (_: java.io.IOException) {
        false
    }

    /** Extract `wine.inf` from APK assets to `<wineRoot>/share/wine/wine.inf`.
     *  wineboot reads this on `--update` to run the PreInstall + DefaultInstall
     *  sections that populate `HKCR\Interface\{IID}\ProxyStubClsid32` (and a
     *  lot of other registry state). Without it, `wineboot --update` exits
     *  immediately with a "failed to update ... wine.inf: No such file or
     *  directory" error and the registry stays incomplete.
     *  Idempotent: rewrites the file each call (cheap, ~150 KB). */
    private fun extractWineInf(ctx: Context, wineRoot: File) {
        if (!assetExists(ctx, "wine.inf")) {
            Log.w(TAG, "wine.inf missing from assets")
            return
        }
        val target = File(wineRoot, "share/wine/wine.inf")
        target.parentFile?.mkdirs()
        try {
            ctx.assets.open("wine.inf").use { input ->
                target.outputStream().use { out -> input.copyTo(out) }
            }
            Log.i(TAG, "extracted wine.inf to ${target.absolutePath} (${target.length()} bytes)")
        } catch (e: Exception) {
            Log.w(TAG, "failed to extract wine.inf: ${e.message}")
        }
    }

    /** Read `wine-fex-manifest.json` and symlink every entry's `path` to its
     *  `libwine_NNNN.so` in nativeLibraryDir. */
    private fun applyManifestSymlinks(ctx: Context, wineRoot: File, nativeLibDir: File) {
        val json = ctx.assets.open("wine-fex-manifest.json").bufferedReader().use { it.readText() }
        val manifest = JSONObject(json)
        val entries = manifest.getJSONArray("entries")
        val wineRootDevice = manifest.getString("wine_root_device")  // "/wine"

        var ok = 0
        var fail = 0
        for (i in 0 until entries.length()) {
            val e = entries.getJSONObject(i)
            val lib = e.getString("lib")
            val devicePath = e.getString("path")  // e.g. /wine/bin/wine
            require(devicePath.startsWith("$wineRootDevice/")) {
                "manifest path $devicePath outside expected root $wineRootDevice"
            }
            // Strip the leading /wine/ — paths under wineRoot are relative.
            val relative = devicePath.removePrefix("$wineRootDevice/")
            val target = File(nativeLibDir, lib)
            val link = File(wineRoot, relative)
            link.parentFile?.mkdirs()
            unlinkIfExists(link)
            try {
                Os.symlink(target.absolutePath, link.absolutePath)
                ok++
            } catch (ex: ErrnoException) {
                Log.w(TAG, "symlink ${link.absolutePath} -> ${target.absolutePath} failed: ${ex.message}")
                fail++
            }
        }
        Log.i(TAG, "manifest symlinks: $ok ok, $fail fail")
    }

    /** Create the WINEPREFIX directory layout. wine's wineboot populates the
     *  rest of drive_c on first run, but we pre-create dosdevices/c: and
     *  dosdevices/z: so paths inside the prefix resolve before wineboot
     *  finishes. Also seed drive_c/windows/system32 with symlinks to our
     *  PE DLLs — wine looks for early-init DLLs like libarm64ecfex.dll
     *  via the explicit `C:\windows\system32\<name>` path which maps to
     *  wineprefix/drive_c/windows/system32/. */
    private fun seedWinePrefix(winePrefix: File) {
        File(winePrefix, "drive_c/windows/system32").mkdirs()
        val dosdev = File(winePrefix, "dosdevices")
        dosdev.mkdirs()
        /* Pre-create a minimal system.reg + user.reg so our seedXxx functions
         * (installUiHostStub, seedActivatableClasses, applyVirtualDesktopRegistry,
         * etc.) can append CLSID/Interface entries BEFORE the first wine run.
         *
         * Without this, the seeding code bails ("system.reg missing → skip")
         * because wine hasn't bootstrapped the prefix yet, and the cloned
         * per-plugin prefix inherits a registry without CLSID_UIHostNoLaunch.
         * X50II then crashes inside effEditOpen on the NULL CoCreateInstance
         * return for that class (write fault at NULL+0x10 = IUnknown::Release
         * vtable slot).
         *
         * Header format matches what wineserver writes (server/registry.c
         * save_all_subkeys): "WINE REGISTRY Version 2" + "All keys relative"
         * + #arch=win64. Wine reads this fine on first run and merges in any
         * other defaults it wants (we end up with the union). */
        val systemReg = File(winePrefix, "system.reg")
        if (!systemReg.exists()) {
            systemReg.writeText(
                "WINE REGISTRY Version 2\n" +
                ";; All keys relative to \\\\Machine\n" +
                "\n#arch=win64\n"
            )
            Log.i(TAG, "pre-seeded minimal system.reg at ${systemReg.absolutePath}")
        }
        val userReg = File(winePrefix, "user.reg")
        if (!userReg.exists()) {
            userReg.writeText(
                "WINE REGISTRY Version 2\n" +
                ";; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000\n" +
                "\n#arch=win64\n"
            )
            Log.i(TAG, "pre-seeded minimal user.reg at ${userReg.absolutePath}")
        }
        val userdefReg = File(winePrefix, "userdef.reg")
        if (!userdefReg.exists()) {
            userdefReg.writeText(
                "WINE REGISTRY Version 2\n" +
                ";; All keys relative to \\\\User\\\\.Default\n" +
                "\n#arch=win64\n"
            )
            Log.i(TAG, "pre-seeded minimal userdef.reg at ${userdefReg.absolutePath}")
        }
        val cDrive = File(dosdev, "c:")
        if (!cDrive.exists()) {
            try {
                Os.symlink("../drive_c", cDrive.absolutePath)
            } catch (e: ErrnoException) {
                Log.w(TAG, "dosdevices/c: symlink failed: ${e.message}")
            }
        }
        // z: -> "/". Without this, wine emits paths like \\?\unix\tmp\foo as
        // the current directory; some JUCE-based plugins recurse on that
        // format trying to walk up to a root and blow the stack inside
        // RtlGetCurrentDirectory_U. Lifted from master branch where the same
        // problem applies.
        val zDrive = File(dosdev, "z:")
        if (!zDrive.exists()) {
            try {
                Os.symlink("/", zDrive.absolutePath)
            } catch (e: ErrnoException) {
                Log.w(TAG, "dosdevices/z: symlink failed: ${e.message}")
            }
        }
    }

    /** Ensure C:\\users\\<user>\\Documents (and the other XDG-equivalents
     *  wineboot normally creates) exist. Anvil.dll's DllMain calls
     *  SHGetFolderPath(MY_DOCUMENTS) and returns FALSE if the path
     *  doesn't resolve — wineboot in our minimal Bionic config skips
     *  these folders, so we mkdir them ourselves. Observed 2026-05-18:
     *  Anvil.dll DllMain returned 0 / LoadLibraryA reported error 998
     *  the moment one of these missing-folder lookups failed. */
    /** Create the standard `C:\Program Files\` and `C:\Program Files (x86)\`
     *  directory tree + matching registry keys so installers (NSIS,
     *  InnoSetup, MSI) don't error out with "can't find Program Files
     *  directory" during InstallationDirectory= resolution. Wine's
     *  wineboot normally creates these, but our minimal setup skips
     *  the full wineboot run.
     *
     *  Idempotent — mkdirs is no-op when dirs exist, and we use the
     *  same v1 marker in user.reg to avoid duplicating registry lines. */
    fun seedProgramFilesDirs(winePrefix: File) {
        val driveC = File(winePrefix, "drive_c")
        if (!driveC.exists()) return
        val dirs = listOf(
            "Program Files",
            "Program Files/Common Files",
            "Program Files (x86)",
            "Program Files (x86)/Common Files",
        )
        for (rel in dirs) {
            File(driveC, rel).mkdirs()
        }
        // Wine's "current version" reg lookup (used by SHGetFolderPath
        // + GetWindowsDirectory + GetSystemWow64Directory) expects these
        // keys. Once seeded, wine returns the right paths to installers.
        val systemReg = File(winePrefix, "system.reg")
        if (!systemReg.exists()) return
        val marker = "vstpoc-program-files-v1"
        val existing = systemReg.readText()
        if (existing.contains(marker)) return
        val body = """

;; $marker
[Software\\Microsoft\\Windows\\CurrentVersion] 1779200000
"CommonFilesDir"="C:\\\\Program Files\\\\Common Files"
"CommonFilesDir (x86)"="C:\\\\Program Files (x86)\\\\Common Files"
"CommonW6432Dir"="C:\\\\Program Files\\\\Common Files"
"ProgramFilesDir"="C:\\\\Program Files"
"ProgramFilesDir (x86)"="C:\\\\Program Files (x86)"
"ProgramW6432Dir"="C:\\\\Program Files"
"ProgramFilesPath"="%ProgramFiles%"

"""
        systemReg.appendText(body)
        Log.i(TAG, "seeded Program Files directories + registry in ${winePrefix.absolutePath}")
    }

    private fun seedUserFolders(ctx: Context, winePrefix: File) {
        val username = "u0_a${ctx.applicationInfo.uid % 100000 - 10000}"
            .takeIf { it.matches(Regex("u\\d+_a\\d+")) }
            ?: System.getProperty("user.name")
            ?: "wine"
        val userDir = File(winePrefix, "drive_c/users/$username")
        if (!userDir.exists()) return  // wineboot owns layout; bail if missing
        val folders = listOf(
            "Documents", "Documents/IRs",
            "Desktop", "Downloads", "Music",
            "Pictures", "Videos", "Contacts", "Favorites",
            "Links", "Searches", "AppData/LocalLow",
        )
        var created = 0
        for (rel in folders) {
            val f = File(userDir, rel)
            if (f.mkdirs()) created++
        }
        Log.i(TAG, "user folders seeded ($created new) under ${userDir.absolutePath}")
    }

    /** Copy bundled .ttf files into wineprefix/drive_c/windows/Fonts/.
     *  Wine's gdi32 enumerates fonts from there and matches by family
     *  name. Plugin UIs that ask for "Tahoma" / "Arial" / "MS Sans Serif"
     *  get the Liberation/DejaVu equivalents we ship — without these,
     *  freetype-built wine has no faces to render with and DrawText
     *  produces blank rectangles.
     *
     *  We also write font-substitution entries so requests by the
     *  Windows names resolve to our Liberation/DejaVu families. Wine
     *  matches HKLM\Software\Microsoft\Windows NT\CurrentVersion\
     *  FontSubstitutes first. */
    private fun seedFonts(ctx: Context, winePrefix: File) {
        val fontsDir = File(winePrefix, "drive_c/windows/Fonts")
        fontsDir.mkdirs()
        var copied = 0
        try {
            val names = ctx.assets.list("wine-fonts") ?: emptyArray()
            for (n in names) {
                if (!n.endsWith(".ttf")) continue
                val dst = File(fontsDir, n)
                if (dst.exists() && dst.length() > 0) continue
                ctx.assets.open("wine-fonts/$n").use { input ->
                    dst.outputStream().use { out -> input.copyTo(out) }
                }
                copied++
            }
            Log.i(TAG, "seedFonts: copied $copied fonts to ${fontsDir.absolutePath} (${names.size} available)")
        } catch (e: Exception) {
            Log.w(TAG, "seedFonts failed: ${e.message}")
        }
        seedFontSubstitutes(winePrefix)
    }

    /** Append HKLM\\Software\\Microsoft\\Windows NT\\CurrentVersion\\
     *  FontSubstitutes mappings to system.reg so wine resolves
     *  "Tahoma" / "Arial" / etc to the Liberation faces we ship. */
    private fun seedFontSubstitutes(winePrefix: File) {
        val systemReg = File(winePrefix, "system.reg")
        if (!systemReg.exists()) return  // wineboot hasn't run yet; reapply next launch
        val marker = "\"Tahoma\"=\"Liberation Sans\""
        val text = systemReg.readText()
        if (text.contains(marker)) return  // already seeded
        /* wineboot's default registry already has a
         *   [Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes]
         * section (with only "MS Shell Dlg"="Tahoma"). Appending another
         * block with the same key name works — wine's registry loader
         * merges entries when sections are listed twice. */
        val body = """

[Software\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes]
"Arial"="Liberation Sans"
"Arial Black"="Liberation Sans"
"Arial Bold"="Liberation Sans Bold"
"Arial Narrow"="Liberation Sans Narrow"
"Courier"="Liberation Mono"
"Courier New"="Liberation Mono"
"Fixedsys"="Liberation Mono"
"Helv"="Liberation Sans"
"Helvetica"="Liberation Sans"
"MS Sans Serif"="Liberation Sans"
"MS Serif"="Liberation Serif"
"MS Shell Dlg"="Liberation Sans"
"MS Shell Dlg 2"="Liberation Sans"
"System"="Liberation Sans"
"Tahoma"="Liberation Sans"
"Times"="Liberation Serif"
"Times New Roman"="Liberation Serif"
"Trebuchet MS"="Liberation Sans"
"Verdana"="Liberation Sans"

"""
        systemReg.appendText(body)
        Log.i(TAG, "seeded FontSubstitutes (Arial/Tahoma/… → Liberation) in system.reg")
    }

    /** Register the RpcSs (Remote Procedure Call) service in system.reg.
     *
     *  Without this, wine's combase logs `err:ole:start_rpcss Failed to
     *  open RpcSs service` whenever a plugin calls CoMarshalInterface,
     *  and dispatches RPC_S_SERVER_UNAVAILABLE — X50II's offline-activation
     *  Register-button handler hits this and silently fails.
     *
     *  The default registration would come from wine.inf's
     *  [DefaultInstall.Services] AddService=RpcSs,0,RpcSsService, but we
     *  deliberately don't extract wine.inf (see the comment above
     *  extractWineInf — wineboot's update_wineprefix triggers winedevice/
     *  menubuilder hooks that don't work in our Bionic env). So we replicate
     *  the [RpcSsService] section's effect directly here. */
    private fun seedRpcSsService(winePrefix: File) {
        val systemReg = File(winePrefix, "system.reg")
        if (!systemReg.exists()) return
        /* Marker includes "ImagePath\"=hex(2):" so any pre-existing entry
         * written in our older "str(2)" format gets re-seeded (the loader
         * was rejecting the older format and dropping the key on first
         * wineserver shutdown). */
        val marker = "ImagePath\"=hex(2):"
        val text = systemReg.readText()
        if (text.contains(marker) &&
            text.contains("[System\\\\CurrentControlSet\\\\Services\\\\RpcSs]")) return
        /* Wine reg format requires a unix-timestamp suffix on the key
         * header — without it the SERVER rewrites system.reg and drops
         * the key on the first wineserver shutdown. ImagePath uses
         * hex(2): (REG_EXPAND_SZ) encoded as UTF-16LE bytes (wine's
         * canonical service path format); plain REG_SZ works in theory
         * but the loader picks up the REG_EXPAND_SZ flag based on type
         * and a downstream %SystemRoot% expansion path expects it. */
        val nowSec = System.currentTimeMillis() / 1000
        val imagePath = "C:\\windows\\system32\\rpcss.exe"
        val expandSzHex = buildString {
            for (ch in imagePath) {
                append("%02x,%02x,".format(ch.code and 0xff, (ch.code ushr 8) and 0xff))
            }
            // NUL terminator (UTF-16LE 0x0000)
            append("00,00")
        }
        val body = """

[System\\CurrentControlSet\\Services\\RpcSs] $nowSec
"ImagePath"=hex(2):$expandSzHex
"DisplayName"="Remote Procedure Call (RPC)"
"Description"="RPC service"
"Type"=dword:00000020
"Start"=dword:00000003
"ErrorControl"=dword:00000001
"ObjectName"="LocalSystem"

"""
        systemReg.appendText(body)
        Log.i(TAG, "seeded RpcSs service registration in system.reg")
    }

    /** Symlink every PE DLL from <wineRoot>/lib/wine/aarch64-windows/ into
     *  <winePrefix>/drive_c/windows/system32/ and likewise for i386-windows
     *  → syswow64/. Wine's loader resolves `C:\windows\system32\<name>.dll`
     *  (and `C:\windows\syswow64\<name>.dll` for WoW64 / 32-bit lookups)
     *  directly to the prefix before the broader search. Without this,
     *  explicit-path lookups (load_arm64ec_module → libarm64ecfex.dll,
     *  wineboot → various builtins, 32-bit ntdll.dll for WoW64) all fail. */
    private fun seedSystem32(wineRoot: File, winePrefix: File) {
        for ((archDir, prefixDir) in listOf(
            "lib/wine/aarch64-windows" to "drive_c/windows/system32",
            "lib/wine/i386-windows"    to "drive_c/windows/syswow64",
        )) {
            val winSrcDir = File(wineRoot, archDir)
            val sys = File(winePrefix, prefixDir)
            sys.mkdirs()
            val files = winSrcDir.list() ?: emptyArray()
            var seeded = 0
            for (name in files) {
                val target = File(winSrcDir, name)
                val link = File(sys, name)
                unlinkIfExists(link)
                try {
                    Os.symlink(target.absolutePath, link.absolutePath)
                    seeded++
                } catch (_: ErrnoException) {}
            }
            Log.i(TAG, "seeded $seeded PE files into $prefixDir")
        }
    }

    /** Drop Microsoft.Windows.Common-Controls 6.0 SxS manifests into the
     *  wineprefix's winsxs/Manifests/ directory so plugins that declare a v6
     *  dependency can satisfy their activation context. See master branch's
     *  history for the painful debugging that converged on this exact
     *  filename + XML version.
     *
     *  Also seed each manifest's matching ASSEMBLY directory under
     *  winsxs/<name-without-.manifest>/ with a comctl32.dll pointing at
     *  the corresponding wine system DLL. Otherwise wine's SxS resolver
     *  hands back the assembly path (e.g. winsxs/x86_…_none_deadbeef/
     *  comctl32.dll) and then fails the actual file-open with
     *  STATUS_DLL_NOT_FOUND — that's task #140. The assembly entry can
     *  be a symlink; wine's loader follows it. */
    fun seedCommonControlsManifests(winePrefix: File) {
        // Lowercase "manifests" — wine ntdll has the path string compiled as
        // "\winsxs\manifests" verbatim. Linux fs is case-sensitive.
        val winsxsDir = File(winePrefix, "drive_c/windows/winsxs")
        val manifestsDir = File(winsxsDir, "manifests").apply { mkdirs() }
        manifestsDir.setReadable(true, false)
        manifestsDir.setExecutable(true, false)
        // Cleanup stale lowercase filenames from earlier iterations.
        manifestsDir.listFiles { f ->
            f.name.startsWith("amd64_microsoft.windows.common-controls") ||
            f.name.startsWith("x86_microsoft.windows.common-controls")
        }?.forEach { it.delete() }
        // Triple: (arch, manifest filename, where to point the assembly's
        // comctl32.dll). amd64 manifests serve 64-bit code → system32 copy;
        // x86 manifests serve 32-bit code → syswow64 copy.
        val variants = listOf(
            Triple(
                "amd64",
                "amd64_Microsoft.Windows.Common-Controls_6595b64144ccf1df_6.0.2600.2982_none_deadbeef",
                "../../system32/comctl32.dll",
            ),
            Triple(
                "x86",
                "x86_Microsoft.Windows.Common-Controls_6595b64144ccf1df_6.0.2600.2982_none_deadbeef",
                "../../syswow64/comctl32.dll",
            ),
        )
        for ((arch, baseName, assemblyTarget) in variants) {
            // The manifest XML itself.
            val manifestFile = File(manifestsDir, "$baseName.manifest")
            val body = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
    <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.2600.2982" processorArchitecture="$arch" publicKeyToken="6595b64144ccf1df" language="*" />
    <file name="comctl32.dll" />
</assembly>
"""
            manifestFile.writeText(body)
            manifestFile.setReadable(true, false)

            // The corresponding assembly directory must contain a real
            // comctl32.dll file the SxS resolver can open. We point a
            // symlink at the existing system32/syswow64 copy so we don't
            // duplicate bytes.
            val assemblyDir = File(winsxsDir, baseName).apply { mkdirs() }
            assemblyDir.setReadable(true, false)
            assemblyDir.setExecutable(true, false)
            val assemblyDll = File(assemblyDir, "comctl32.dll")
            if (assemblyDll.exists() || java.nio.file.Files.isSymbolicLink(assemblyDll.toPath())) {
                try { java.nio.file.Files.delete(assemblyDll.toPath()) } catch (_: Exception) {}
            }
            try {
                Os.symlink(assemblyTarget, assemblyDll.absolutePath)
            } catch (e: ErrnoException) {
                Log.w(TAG, "seedCommonControlsManifests: symlink $assemblyDll failed: ${e.message}")
            }
        }
    }

    /** Tell wine to report Win7 (6.1 SP1, build 7601) instead of its default
     *  Win10. Some plugins (e.g. X50II_x64) probe the Windows version, take
     *  the modern branch, and call WinRT factories like
     *  Windows.UI.ViewManagement.UIViewSettings. Wine has no WinRT, so the
     *  factory returns NULL, the plugin doesn't check, and effEditOpen
     *  segfaults on a null-pointer write. Win7 mode steers them onto the
     *  legacy GetDpiForMonitor / SetProcessDPIAware path that wine handles
     *  fine. Idempotent — appends [Software\\Wine] with Version=win7 if not
     *  already present in user.reg. */
    /** Seed PATH, ComSpec, PATHEXT, TEMP, TMP into system.reg's
     *  Session Manager\Environment key. Without these, Electron-based apps
     *  (IK Multimedia Product Manager, Native Access, etc.) fail at first
     *  launch — Node's libuv resolves cmd.exe via the Windows-side PATH env
     *  var when spawnSync is called from JS; with no PATH, lookup returns
     *  ENOENT and the JS code throws "spawnSync cmd.exe ENOENT" before any
     *  UI shows.
     *
     *  VST plugins don't typically spawn child processes, so they never hit
     *  this — which is why the gap stayed hidden until executable hosting
     *  landed. Idempotent via the v1 marker. */
    fun seedDefaultEnvironment(winePrefix: File) {
        val systemReg = File(winePrefix, "system.reg")
        if (!systemReg.exists()) return
        val marker = "vstpoc-default-env-v1"
        val text = systemReg.readText()
        if (text.contains(marker)) return
        val body = """

#$marker
[System\\CurrentControlSet\\Control\\Session Manager\\Environment]
"ComSpec"="C:\\windows\\system32\\cmd.exe"
"Path"=str(2):"C:\\windows\\system32;C:\\windows;C:\\windows\\System32\\Wbem"
"PATHEXT"=".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC"
"TEMP"=str(2):"C:\\windows\\temp"
"TMP"=str(2):"C:\\windows\\temp"
"OS"="Windows_NT"
"windir"=str(2):"C:\\windows"

"""
        systemReg.appendText(body)
        Log.i(TAG, "seeded default Session Manager\\Environment in ${systemReg.absolutePath}")
    }

    fun seedWindowsVersion(winePrefix: File) {
        val userReg = File(winePrefix, "user.reg")
        // STRIP any existing `Software\Wine\Version=winNN` entry. Wine's
        // ntdll/version.c:482 hardcodes the build number for each named
        // version (e.g. win11 → 10.0.22000 RTM). When that key is set, wine
        // IGNORES our Software\Microsoft\Windows NT\CurrentVersion seed
        // (which has the more accurate build 22621 for Win11 22H2). Modern
        // Inno-Setup installers like TONEX 1.12 check
        // dwBuildNumber >= 22621 via GetVersionExW; with the hardcoded
        // 22000 they reject the OS as "wrong Windows version".
        //
        // Removing the key entirely makes wine fall through to its registry-
        // read fallback (get_nt_registry_version), which reads our
        // CurrentMajor/Minor/BuildNumber values directly. Result: GetVersionEx
        // returns 10.0.22621 — what every modern installer expects.
        if (userReg.exists()) {
            val text = userReg.readText()
            val versionLines = text.lines().filter {
                it.trim().matches(Regex("^\"Version\"=\"win\\w+\"$"))
            }
            if (versionLines.isNotEmpty()) {
                val cleaned = text.lines()
                    .filterNot { it.trim().matches(Regex("^\"Version\"=\"win\\w+\"$")) }
                    .joinToString("\n")
                userReg.writeText(cleaned)
                Log.i(TAG, "stripped Software\\Wine\\Version (was: $versionLines) " +
                           "so wine reads registry block instead of hardcoded build")
            }
        }

        /* Wine's Software\Wine\Version setting tells wine WHICH Windows
         * version to spoof, but it does NOT auto-populate the
         * Software\Microsoft\Windows NT\CurrentVersion keys that many
         * installers query directly (instead of GetVersionExW). On a
         * vanilla prefix those keys are empty; modern installers (TONEX,
         * Native Access, recent AmpliTube) probe ProductName /
         * CurrentMajorVersionNumber and reject the install if blank or
         * if the value doesn't reflect Windows 10+. Write the canonical
         * Win10 22H2 (build 19045) values to system.reg so version
         * checks pass. */
        val systemReg = File(winePrefix, "system.reg")
        if (!systemReg.exists()) return
        val sysText = systemReg.readText()
        if (sysText.contains("vstpoc-win11-currentversion-v1")) return
        /* Use wine's internal `hex(4):` format for dword values instead of
         * the plain `dword:` form. wineserver's reg parser strips unknown
         * dword entries during the load/save round-trip (we observed
         * CurrentMajorVersionNumber + CurrentMinorVersionNumber going
         * missing after the first wine launch even though ProductName /
         * EditionID etc. survived). The hex(4) form is what wineserver
         * itself writes out, so the round-trip preserves it.
         *
         * Win10 build 19045 = 0x4A65. Little-endian dword: 65,4a,00,00. */
        /* Use wine's internal `hex(4):` format for dword values instead of
         * the plain `dword:` form. wineserver's reg parser strips unknown
         * dword entries during the load/save round-trip; the hex(4) form
         * is what wineserver itself writes out, so the round-trip
         * preserves it. Win10 build 19045 = 0x4A65. */
        val sysBody = """

#vstpoc-win11-currentversion-v1
[Software\\Microsoft\\Windows NT\\CurrentVersion]
"ProductName"="Windows 11 Pro"
"CurrentVersion"="10.0"
"CurrentMajorVersionNumber"=hex(4):0a,00,00,00
"CurrentMinorVersionNumber"=hex(4):00,00,00,00
"CurrentBuild"="22621"
"CurrentBuildNumber"="22621"
"UBR"=hex(4):ce,07,00,00
"BuildLab"="22621.ni_release.220506-1250"
"BuildLabEx"="22621.1.amd64fre.ni_release.220506-1250"
"EditionID"="Professional"
"InstallationType"="Client"
"ProductId"="00000-00000-00000-00000"
"ReleaseId"="2009"
"DisplayVersion"="22H2"
"SoftwareType"="System"
"PathName"="C:\\\\windows"
"SystemRoot"="C:\\\\windows"

#vstpoc-amd64-arch-v1
[System\\CurrentControlSet\\Control\\Session Manager\\Environment]
"PROCESSOR_ARCHITECTURE"="AMD64"
"PROCESSOR_IDENTIFIER"="Intel64 Family 6 Model 158 Stepping 9, GenuineIntel"
"PROCESSOR_LEVEL"="6"
"PROCESSOR_REVISION"="9e09"

"""
        systemReg.appendText(sysBody)
        Log.i(TAG, "seeded Windows NT CurrentVersion (Win10 22H2/19045) in ${systemReg.absolutePath}")
    }

    /** Disable wine's Direct3D DLLs so GPU-rendering plugins fall back to
     *  GDI (or fail to load with a clear error) instead of crashing.
     *
     *  Background: AmpCraft and similar JUCE-7-era plugins create a D3D11
     *  device for UI rendering. wined3d (wine's D3D→GL translator) tries
     *  to init an OpenGL context, but our X server doesn't implement GLX
     *  and our wine build has no Vulkan support either, so
     *  wined3d_adapter_gl_init returns a NULL adapter. The plugin doesn't
     *  null-check and SIGSEGVs reading address 0x0 — the Windows-style
     *  crash dialog appears with no editor.
     *
     *  Setting DllOverrides "d3d11"/"d3d10"/"d3d9"/"d3d8"/"d3dcompiler_*"
     *  to empty string makes LoadLibrary("d3d11.dll") fail cleanly with
     *  ERROR_MOD_NOT_FOUND. Plugins that detect this and have a fallback
     *  path (most JUCE plugins) will use GDI. Plugins that hard-require
     *  D3D will fail to load — that's still better than a crash. */
    /** Disable winemenubuilder.exe so installer "Create shortcuts" steps
     *  don't hang trying to write .desktop files to /data/.local — that
     *  path doesn't exist (and we can't create it) under untrusted_app_27
     *  SELinux. Wine launches menubuilder for every shortcut creation
     *  call from InnoSetup/NSIS; without this override it spins up,
     *  fails to write each icon/.desktop file, then never exits cleanly,
     *  leaving the installer waiting on its child process.
     *
     *  Empty-string override = "this DLL/.exe is not available". Wine's
     *  CreateProcess returns failure cleanly; installers treat the
     *  shortcut creation as best-effort and proceed.
     *
     *  Idempotent via the v1 marker. Per-prefix because installer
     *  templates clone from base + may also need this re-applied. */
    fun seedDisableMenubuilder(winePrefix: File) {
        val userReg = File(winePrefix, "user.reg")
        if (!userReg.exists()) return
        val marker = "vstpoc-disable-menubuilder-v1"
        val text = userReg.readText()
        if (text.contains(marker)) return
        val body = """

#$marker
[Software\\Wine\\DllOverrides]
"winemenubuilder.exe"=""

"""
        userReg.appendText(body)
        Log.i(TAG, "seeded DllOverrides disable-menubuilder in ${userReg.absolutePath}")
    }

    fun seedDisableDirect3D(winePrefix: File) {
        val userReg = File(winePrefix, "user.reg")
        if (!userReg.exists()) return
        val text = userReg.readText()
        // v7 — wine was rebuilt with --with-vulkan and DXVK 2.5.3 x64 DLLs
        // are now installed into system32 by installDxvk(). DllOverrides
        // point d3d9/d3d10core/d3d11/dxgi at "native,builtin" so wine
        // prefers DXVK's DLLs (native = installed in system32) but falls
        // back to its own builtins if DXVK fails to load. wined3d stays
        // builtin since DXVK's d3d9.dll doesn't replace d3d8/d3d10 and
        // wine's d3d8 still routes through wined3d. d3dcompiler_* stays
        // builtin (it's just shader compilation, not GL-bound).
        val marker = "vstpoc-d3d-overrides-v7"
        if (text.contains(marker)) return
        val body = """

#$marker
[Software\\Wine\\DllOverrides]
"d3d8"="builtin"
"d3d9"="native,builtin"
"d3d10"="native,builtin"
"d3d10_1"="native,builtin"
"d3d10core"="native,builtin"
"d3d11"="native,builtin"
"d3d12"="builtin"
"d3d12core"="builtin"
"d3dcompiler_33"="builtin"
"d3dcompiler_34"="builtin"
"d3dcompiler_35"="builtin"
"d3dcompiler_36"="builtin"
"d3dcompiler_37"="builtin"
"d3dcompiler_38"="builtin"
"d3dcompiler_39"="builtin"
"d3dcompiler_40"="builtin"
"d3dcompiler_41"="builtin"
"d3dcompiler_42"="builtin"
"d3dcompiler_43"="builtin"
"d3dcompiler_46"="builtin"
"d3dcompiler_47"="builtin"
"wined3d"="builtin"
"dxgi"="native,builtin"

"""
        userReg.appendText(body)
        Log.i(TAG, "seeded DllOverrides v2 (d3d* disabled, dxgi=builtin) in ${userReg.absolutePath}")
    }

    /** Copy bundled uihost_stub_{x64,x86}.dll from APK assets into the
     *  prefix's system32 / syswow64 and register CLSID_UIHostNoLaunch
     *  in HKCR (= system.reg [Software\\Classes\\CLSID\\…]) so plugins
     *  that CoCreateInstance Windows' touch-keyboard host get a no-op
     *  ITipInvocation back instead of NULL.
     *
     *  Background: X50II_x64 (and likely other modern-Windows-shell
     *  plugins) calls CoCreateInstance(CLSID_UIHostNoLaunch, ...) in
     *  effEditOpen and doesn't check the HRESULT. Wine returns
     *  REGDB_E_CLASSNOTREG and leaves the out-pointer NULL, the plugin
     *  dereferences it, the editor thread page-faults, WineDbg attaches
     *  and the editor never appears. The stub DLL (built by
     *  scripts/build-uihost-stub.sh) implements IClassFactory →
     *  ITipInvocation::Toggle() = S_OK so the call chain completes.
     *  Idempotent. */
    /** Copy DXVK's prebuilt d3d9/d3d10core/d3d11/dxgi DLLs from APK assets
     *  into the wineprefix's system32. DXVK translates Direct3D 9/10/11
     *  calls to Vulkan, sidestepping wine's wined3d which is desktop-GL-
     *  only (and our wine has --without-opengl in practice since the X
     *  server's GLX scaffolding isn't a real GL implementation). Android
     *  has libvulkan.so natively; wine was rebuilt with --with-vulkan so
     *  winevulkan.so initialises successfully against it.
     *
     *  DllOverrides in seedDisableDirect3D's v7 marker point d3d11/d3d10/
     *  d3d9/dxgi at "native" → wine prefers these DXVK DLLs over its own
     *  builtin (wined3d-backed) versions. */
    fun installDxvk(ctx: Context, winePrefix: File) {
        val dlls = listOf("d3d9.dll", "d3d10core.dll", "d3d11.dll", "dxgi.dll")
        val sys32 = File(winePrefix, "drive_c/windows/system32")
        sys32.mkdirs()
        // No skip-if-marker logic: WineSetup.seedSystem32 runs on every
        // Play press and recreates wine-builtin symlinks in system32,
        // which clobbers DXVK. We must reinstall every play so the
        // bytes are the DXVK ones, not wine's wined3d-backed builtins.
        // Cost is ~12 MB of copies from assets — negligible vs the
        // full wine startup time.
        for (dll in dlls) {
            try {
                val dst = File(sys32, dll)
                // The wineprefix's system32 already has SYMLINKS to wine's
                // own builtin d3d11.dll/dxgi.dll/etc. We need to replace
                // them with regular files containing DXVK's bytes — the
                // dst.exists() check returns true for symlinks AND a write
                // through the symlink would target wine's read-only install
                // directory. unlinkIfExists() removes the symlink first.
                unlinkIfExists(dst)
                ctx.assets.open("dxvk/x64/$dll").use { input ->
                    dst.outputStream().use { out -> input.copyTo(out) }
                }
                Log.i(TAG, "installed DXVK $dll → ${dst.absolutePath} (${dst.length()} bytes)")
            } catch (e: Exception) {
                Log.w(TAG, "installDxvk: failed to copy $dll: ${e.message}")
            }
        }
    }

    fun installUiHostStub(ctx: Context, winePrefix: File) {
        val mappings = listOf(
            "uihost_stub_x64.dll" to "drive_c/windows/system32",
            "uihost_stub_x86.dll" to "drive_c/windows/syswow64",
        )
        for ((asset, prefixDir) in mappings) {
            try {
                val dst = File(winePrefix, "$prefixDir/uihost_stub.dll")
                dst.parentFile?.mkdirs()
                // Always overwrite — the bundled .dll in APK assets is the
                // source of truth, and we evolve its export table (added
                // CLSID_VirtualDesktopManager support after the initial
                // ITipInvocation-only release). The skip-if-exists logic
                // we had before left old stubs in place on existing
                // wineprefix clones, defeating the registry pattern we
                // bumped to v2.
                ctx.assets.open(asset).use { input ->
                    dst.outputStream().use { out -> input.copyTo(out) }
                }
                Log.i(TAG, "installed $asset → ${dst.absolutePath} (${dst.length()} bytes)")
            } catch (e: Exception) {
                Log.w(TAG, "installUiHostStub: failed to copy $asset: ${e.message}")
            }
        }

        // Registry entries: HKCR\CLSID\{4ce576fa-...} → InprocHandler32 →
        // c:\windows\system32\uihost_stub.dll. Wine's WoW64 redirector
        // rewrites that to syswow64 for 32-bit callers automatically.
        // The marker detects the *new* InprocHandler32 format — earlier
        // builds wrote LocalServer32 which wine tried to exec as an .exe
        // and the call returned NULL. If the old marker is present we
        // overwrite with the new format.
        val systemReg = File(winePrefix, "system.reg")
        if (!systemReg.exists()) return
        // Marker keys on the threading model: "Apartment" required cross-
        // apartment marshaling that needs RpcSs — which our minimal
        // wineboot doesn't run, so CoCreateInstance failed with E_NOINTERFACE
        // and the plugin still crashed. "Both" lets the factory be used
        // directly from any thread, no marshal needed.
        val newMarker = "ThreadingModel\"=\"Both\""
        // v2 marker: also includes CLSID_VirtualDesktopManager registration.
        // Bumped so prefixes seeded by the previous build pick this up.
        val v2Marker = "vstpoc-com-stubs-v2"
        val existing = systemReg.readText()
        if (existing.contains(v2Marker)) return
        // Plugins (X50II in particular) call CoCreateInstance with
        // CLSCTX = INPROC_HANDLER | LOCAL_SERVER (0x6) — they do NOT
        // request INPROC_SERVER (0x1). So InprocServer32 alone is
        // ignored. We register InprocHandler32 as our primary entry
        // since wine treats the handler as a full in-process server
        // when no real out-of-process server exists. InprocServer32
        // stays as a fallback for callers that include CLSCTX_INPROC_SERVER.
        // (LocalServer32 deliberately omitted: it would point at our
        // DLL, wine would try to exec it as an .exe, fail, then leave
        // the call NULL — the very bug we are fixing.)
        val body = """

#$v2Marker
[Software\\Classes\\CLSID\\{4ce576fa-83dc-4f88-951c-9d0782b4e376}]
@="UIHostNoLaunch"

[Software\\Classes\\CLSID\\{4ce576fa-83dc-4f88-951c-9d0782b4e376}\\InprocHandler32]
@="C:\\windows\\system32\\uihost_stub.dll"
"ThreadingModel"="Both"

[Software\\Classes\\CLSID\\{4ce576fa-83dc-4f88-951c-9d0782b4e376}\\InprocServer32]
@="C:\\windows\\system32\\uihost_stub.dll"
"ThreadingModel"="Both"

[Software\\Classes\\Interface\\{37c994e7-432b-4834-a2f7-dce1f13b834b}]
@="ITipInvocation"

; AmpCraft (and likely other JUCE-7 plugins) calls
; CoCreateInstance(CLSID_VirtualDesktopManager, ...) to detect which
; Windows virtual desktop its top-level window belongs to. Wine has no
; twinapi.dll; without registration, CoCreateInstance returns
; CLASS_E_CLASSNOTREG (0x80040154), the plugin doesn't check, derefs
; the NULL out-pointer at vtable offset 0x10 (IUnknown::Release slot)
; and SIGSEGV's a few seconds after the editor opens.
;
; Register the CLSID against our existing uihost_stub.dll. The stub
; returns S_OK from DllGetClassObject with a generic IClassFactory.
; CreateInstance for IID_IVirtualDesktopManager will return E_NOINTERFACE
; (the stub only implements ITipInvocation), but the plugin gets back a
; proper failure HRESULT instead of a stale NULL pointer that bypasses
; its check. A handful of plugins crash anyway because they don't check
; HRESULT — TODO: extend uihost_stub.dll with a real IVirtualDesktopManager
; that returns sensible defaults (IsWindowOnCurrentVirtualDesktop=TRUE,
; GetWindowDesktopId=zero-GUID).
[Software\\Classes\\CLSID\\{aa509086-5ca9-4c25-8f95-589d3c07b48a}]
@="VirtualDesktopManager"

[Software\\Classes\\CLSID\\{aa509086-5ca9-4c25-8f95-589d3c07b48a}\\InprocHandler32]
@="C:\\windows\\system32\\uihost_stub.dll"
"ThreadingModel"="Both"

[Software\\Classes\\CLSID\\{aa509086-5ca9-4c25-8f95-589d3c07b48a}\\InprocServer32]
@="C:\\windows\\system32\\uihost_stub.dll"
"ThreadingModel"="Both"

[Software\\Classes\\Interface\\{a5cd92ff-29be-454c-8d04-d82879fb3f1b}]
@="IVirtualDesktopManager"

"""
        systemReg.appendText(body)
        Log.i(TAG, "seeded CLSID_UIHostNoLaunch + VirtualDesktopManager in ${systemReg.absolutePath}")
    }

    /** Register the WinRT runtime classes that wine ships in windows.ui.dll
     *  but our minimal wineboot doesn't auto-register. RoGetActivationFactory
     *  looks the class up in HKLM\Software\Microsoft\WindowsRuntime\
     *  ActivatableClassId\<classname> for a DllPath, then LoadLibrary's that
     *  DLL and calls DllGetActivationFactory. Without the registry entry the
     *  whole call fails with "Failed to find library" (observed with X50II,
     *  which then dereferences the NULL and crashes effEditOpen).
     *  Seeded into system.reg under [Software\\Microsoft\\WindowsRuntime\\
     *  ActivatableClassId\\<class>] (HKLM root is implicit in system.reg).
     *  Idempotent. */
    fun seedActivatableClasses(winePrefix: File) {
        val systemReg = File(winePrefix, "system.reg")
        if (!systemReg.exists()) return
        val text = systemReg.readText()
        val marker = "[Software\\\\Microsoft\\\\WindowsRuntime\\\\ActivatableClassId\\\\Windows.UI.ViewManagement.UIViewSettings]"
        if (text.contains(marker)) return
        // Threading=0 (MTA) is what wine's windows.ui.dll declares for its
        // UIViewSettings stub; TrustLevel=0 (BaseTrust) matches.
        val body = """

$marker
"DllPath"="C:\\windows\\system32\\windows.ui.dll"
"Threading"=dword:00000000
"TrustLevel"=dword:00000000

[Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.UI.ViewManagement.UISettings]
"DllPath"="C:\\windows\\system32\\windows.ui.dll"
"Threading"=dword:00000000
"TrustLevel"=dword:00000000

[Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\Windows.UI.ViewManagement.InputPane]
"DllPath"="C:\\windows\\system32\\windows.ui.dll"
"Threading"=dword:00000000
"TrustLevel"=dword:00000000

"""
        systemReg.appendText(body)
        Log.i(TAG, "seeded WinRT ActivatableClassId entries in ${systemReg.absolutePath}")
    }

    /** Public entry point for MainActivity to call after wineboot has created
     *  user.reg. Force virtual-desktop mode so wine's winex11.drv creates a
     *  real X11 window for the plugin editor. */
    /** Run `wine wineboot.exe --update` once against the base wineprefix
     *  to trigger wine.inf's RegisterDllsSection. Without this step the
     *  prefix's `system.reg` ends up with ZERO `[HKCR\Interface\{IID}\
     *  ProxyStubClsid32]` entries — so `CoMarshalInterface` returns
     *  E_NOINTERFACE for any non-IUnknown interface, and any plugin that
     *  uses RegisterDragDrop or activates a WinRT class never finishes
     *  showing its popup/menu/dropdown.
     *
     *  Gated on `<prefix>/.vstpoc-wineboot-update-v2`. Skips if present.
     *  Blocks the caller (wine subprocess + 120 s timeout) — call from
     *  a background thread, NOT the UI thread. Returns true on a clean
     *  exit (0); false on any failure (the per-prefix marker is not
     *  created on failure, so the next launch retries). */
    fun runWinebootUpdateOnce(ctx: Context, setup: Setup): Boolean {
        val marker = File(setup.winePrefix, ".vstpoc-wineboot-update-v2")
        if (marker.exists()) {
            Log.i(TAG, "runWinebootUpdateOnce: marker present, skip")
            return true
        }
        if (!setup.winePrefix.exists()) {
            Log.w(TAG, "runWinebootUpdateOnce: prefix missing at ${setup.winePrefix.absolutePath}; skip")
            return false
        }
        val wineBinary = File(setup.wineRoot, "bin/wine").absolutePath
        val wineserverBinary = File(setup.wineRoot, "bin/wineserver").absolutePath
        val wineDllPath = File(setup.wineRoot, "lib/wine/aarch64-windows").absolutePath
        val nativeLibDir = ctx.applicationInfo.nativeLibraryDir.orEmpty()
        val cacheDir = ctx.cacheDir.absolutePath

        Log.i(TAG, "runWinebootUpdateOnce: launching wineboot --update against ${setup.winePrefix.absolutePath}")
        val started = System.currentTimeMillis()
        val rc = try {
            com.varcain.vsthost.NativeBridge.nativeRunWineboot(
                prefixPath        = setup.winePrefix.absolutePath,
                wineBinary        = wineBinary,
                wineserverBinary  = wineserverBinary,
                wineDllPath       = wineDllPath,
                nativeLibDir      = nativeLibDir,
                cacheDir          = cacheDir,
                timeoutSec        = 180,
            )
        } catch (e: Throwable) {
            Log.e(TAG, "runWinebootUpdateOnce: JNI threw", e)
            return false
        }
        val took = System.currentTimeMillis() - started
        Log.i(TAG, "runWinebootUpdateOnce: rc=$rc in ${took}ms")
        if (rc != 0) {
            Log.w(TAG, "runWinebootUpdateOnce: wineboot returned $rc; not creating marker (will retry next launch)")
            return false
        }
        runCatching { marker.writeText("vstpoc-wineboot-update-v2\n") }
            .onFailure { Log.w(TAG, "runWinebootUpdateOnce: failed to write marker: ${it.message}") }
        return true
    }

    fun applyVirtualDesktopRegistry(ctx: Context) {
        val winePrefix = File(ctx.filesDir, "wineprefix")
        // seedFontSubstitutes needs system.reg to exist — wineboot writes
        // it on first launch, so the first WineSetup.ensure call no-ops it.
        // Call again here (post-first-boot from MainActivity) to land the
        // mappings before the next plugin loads.
        seedFontSubstitutes(winePrefix)
        seedWindowsVersion(winePrefix)
        seedDisableDirect3D(winePrefix)
        seedDisableMenubuilder(winePrefix)
        seedActivatableClasses(winePrefix)
        installUiHostStub(ctx, winePrefix)
        val userReg = File(winePrefix, "user.reg")
        if (!userReg.exists()) {
            Log.w(TAG, "user.reg missing at ${userReg.absolutePath}; skipping virtual-desktop registry")
            return
        }
        val existing = userReg.readText()
        val explorerKey = "[Software\\\\Wine\\\\Explorer]"
        val desktopsKey = "[Software\\\\Wine\\\\Explorer\\\\Desktops]"
        if (existing.contains(explorerKey) && existing.contains(desktopsKey)) {
            return
        }
        val append = """

$explorerKey
"Desktop"="vstpoc"

$desktopsKey
"vstpoc"="800x130"

"""
        userReg.appendText(append)
        Log.i(TAG, "appended virtual-desktop registry to ${userReg.absolutePath}")
    }

    private fun unlinkIfExists(f: File) {
        try {
            // lstat throws ENOENT if it doesn't exist; otherwise we have
            // something to remove. File.delete() does unlink(2) underneath;
            // for symlinks it removes the link, not the target.
            Os.lstat(f.absolutePath)
            f.delete()
        } catch (_: ErrnoException) {
            // doesn't exist; nothing to do.
        }
    }
}
