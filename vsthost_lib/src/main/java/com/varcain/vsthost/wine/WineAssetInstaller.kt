package com.varcain.vsthost.wine

import android.content.Context
import android.content.res.AssetManager
import android.os.StatFs
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.util.Log
import com.varcain.vsthost.util.deleteTreeNoFollow
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.security.MessageDigest

/**
 * Installs a schema-2 manifest's "asset" (PE) entries into the wine root as
 * read-only regular files.
 *
 * Why this works outside nativeLibraryDir: the full flavor targets SDK 28, whose
 * SELinux domain (untrusted_app_27) grants execute + execmod on app_data_file,
 * so wine can map and relocate PE images extracted into filesDir (verified
 * on-device: ntdll/kernel32/… mapped r-x from files/wine/lib/wine/{aarch64,i386}-windows).
 *
 * Files are 0444: prefixes reach them through system32/syswow64 symlinks, and an
 * installer writing through such a link must not modify the copy shared by every
 * prefix. That's the behaviour wine (and patch 0054) relied on when the targets
 * were read-only files in nativeLibraryDir — the write fails and wine replaces
 * the link instead.
 */
internal object WineAssetInstaller {
    private const val TAG = "WineSetup"
    private const val STATE_FILE = ".pe-installed.json"
    private const val STAGING_DIR = ".staging"
    private const val READ_ONLY = 0b100_100_100  // 0444
    private const val FREE_SPACE_MARGIN = 64L shl 20

    /**
     * Make every asset entry present under [wineRoot] with the manifest's bytes.
     * Fast path (pack digest unchanged): lstat-only integrity check per file.
     * Otherwise re-extract the entries whose recorded sha256 changed or whose
     * file is missing / not a 0444 regular file of the right size, verifying
     * sha256 while streaming. Finally drop files the manifest no longer lists.
     * [onProgress] is called with (extracted, toExtract) before and after each
     * extracted file; not at all on the fast path.
     */
    fun install(
        ctx: Context,
        wineRoot: File,
        manifest: WineRuntimeManifest,
        onProgress: (done: Int, total: Int) -> Unit = { _, _ -> },
    ) {
        if (manifest.assets.isEmpty()) return
        val t0 = System.currentTimeMillis()
        val stateFile = File(wineRoot, STATE_FILE)
        val (installedDigest, installedShas) = readState(stateFile)
        val digestUnchanged = installedDigest != null && installedDigest == manifest.packDigest
        val todo = manifest.assets.filter { e ->
            !isIntact(File(wineRoot, e.relPath), e.size) ||
                (!digestUnchanged && installedShas[e.relPath] != e.sha256)
        }

        if (todo.isNotEmpty()) {
            val needed = todo.sumOf { it.size }
            val free = StatFs(wineRoot.absolutePath).availableBytes
            if (free < needed + FREE_SPACE_MARGIN) {
                throw IOException("not enough storage for the Windows runtime: " +
                    "need ${(needed + FREE_SPACE_MARGIN) shr 20} MiB, have ${free shr 20} MiB")
            }
            val staging = File(wineRoot, STAGING_DIR)
            staging.deleteTreeNoFollow()
            staging.mkdirs()
            onProgress(0, todo.size)
            todo.forEachIndexed { i, e ->
                extract(ctx, e, File(staging, "$i.tmp"), File(wineRoot, e.relPath))
                onProgress(i + 1, todo.size)
            }
            staging.deleteTreeNoFollow()
        }
        val removed = removeOrphans(wineRoot, manifest)
        if (todo.isNotEmpty() || removed > 0 || !digestUnchanged) {
            writeState(stateFile, manifest)
        }
        Log.i(TAG, "PE assets: ${manifest.assets.size} listed, ${todo.size} extracted " +
            "(${todo.sumOf { it.size } shr 20} MiB), $removed orphans removed, " +
            "in ${System.currentTimeMillis() - t0} ms")
    }

    /** Stream one asset to [tmp] (hashing as we go), verify, make it 0444 and
     *  atomically rename it over [dst] — replacing a legacy symlink there
     *  without following it. */
    private fun extract(ctx: Context, e: WineRuntimeManifest.AssetEntry, tmp: File, dst: File) {
        val sha = MessageDigest.getInstance("SHA-256")
        ctx.assets.open(e.asset, AssetManager.ACCESS_STREAMING).use { input ->
            FileOutputStream(tmp).use { out ->
                val buf = ByteArray(1 shl 16)
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    sha.update(buf, 0, n)
                    out.write(buf, 0, n)
                }
                out.fd.sync()
            }
        }
        val got = sha.digest().joinToString("") { "%02x".format(it) }
        if (got != e.sha256 || tmp.length() != e.size) {
            tmp.delete()
            throw IOException("asset ${e.asset} is corrupt (sha256 $got, ${tmp.length()} bytes; " +
                "manifest ${e.sha256}, ${e.size} bytes)")
        }
        Os.chmod(tmp.absolutePath, READ_ONLY)
        dst.parentFile?.mkdirs()
        Os.rename(tmp.absolutePath, dst.absolutePath)
    }

    private fun isIntact(f: File, size: Long): Boolean = try {
        val st = Os.lstat(f.absolutePath)
        OsConstants.S_ISREG(st.st_mode) && st.st_size == size && (st.st_mode and 0b111_111_111) == READ_ONLY
    } catch (_: ErrnoException) {
        false
    }

    /** Delete entries in the asset directories that the manifest doesn't list
     *  (e.g. DLLs dropped from the pack). Returns how many were removed. */
    private fun removeOrphans(wineRoot: File, manifest: WineRuntimeManifest): Int {
        val expected = manifest.assets.groupBy({ File(it.relPath).parent ?: "" }, { File(it.relPath).name })
        var removed = 0
        for ((dir, names) in expected) {
            val keep = names.toHashSet()
            File(wineRoot, dir).listFiles()?.forEach { f ->
                if (f.name !in keep && !f.isDirectory && f.delete()) removed++
            }
        }
        return removed
    }

    private fun readState(f: File): Pair<String?, Map<String, String>> = try {
        val j = JSONObject(f.readText())
        val files = j.getJSONObject("files")
        j.optString("pack_digest").ifEmpty { null } to files.keys().asSequence().associateWith { files.getString(it) }
    } catch (_: Exception) {
        null to emptyMap()
    }

    private fun writeState(f: File, manifest: WineRuntimeManifest) {
        val files = JSONObject()
        manifest.assets.forEach { files.put(it.relPath, it.sha256) }
        val tmp = File(f.parentFile, "${f.name}.tmp")
        tmp.writeText(JSONObject().put("pack_digest", manifest.packDigest ?: "").put("files", files).toString())
        Os.rename(tmp.absolutePath, f.absolutePath)
    }
}
