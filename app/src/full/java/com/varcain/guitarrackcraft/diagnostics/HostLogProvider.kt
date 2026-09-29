/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.varcain.guitarrackcraft.diagnostics

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import java.io.File
import java.io.FileNotFoundException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Read-only access to the wine launcher's logs for `adb shell`. They are
 * written to the app's private cache (WineHostProcess: vst_host_<plugin>.log
 * per host, vst_host_installer.log, wineboot.log, rpcss.log, turnip.log,
 * msi_install.log), which adb can't read on a release build (no run-as).
 *
 * The manifest guards the provider with android.permission.DUMP: the adb
 * shell and the system hold it, apps can't obtain it. Only those log files
 * are served, by bare name; nothing can be written.
 *
 *   adb shell content query --uri content://com.varcain.guitarrackcraft.hostlogs/
 *   adb shell content read --uri content://com.varcain.guitarrackcraft.hostlogs/<name>
 */
class HostLogProvider : ContentProvider() {

    override fun onCreate(): Boolean = true

    private fun logDir(): File = requireNotNull(context).cacheDir

    private fun isLogName(name: String): Boolean = LOG_NAME.matches(name)

    /** Lists the logs, newest first: name, size in bytes, last modified. */
    override fun query(
        uri: Uri,
        projection: Array<out String>?,
        selection: String?,
        selectionArgs: Array<out String>?,
        sortOrder: String?,
    ): Cursor {
        val cursor = MatrixCursor(arrayOf("name", "size", "modified"))
        val format = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US)
        logDir().listFiles { f -> f.isFile && isLogName(f.name) }
            ?.sortedByDescending { it.lastModified() }
            ?.forEach { cursor.addRow(arrayOf(it.name, it.length(), format.format(Date(it.lastModified())))) }
        return cursor
    }

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        if (mode != "r") throw SecurityException("host logs are read-only")
        val name = uri.lastPathSegment
        if (name == null || !isLogName(name)) throw FileNotFoundException("not a host log: $uri")
        val file = File(logDir(), name)
        if (!file.isFile) throw FileNotFoundException("no such log: $name")
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)
    }

    override fun getType(uri: Uri): String = "text/plain"

    override fun insert(uri: Uri, values: ContentValues?): Uri? =
        throw UnsupportedOperationException("read-only")

    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?): Int =
        throw UnsupportedOperationException("read-only")

    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?): Int =
        throw UnsupportedOperationException("read-only")

    private companion object {
        /** The launcher's log names; no path separators or "..", so a name
         *  can only address a file directly in the cache directory. */
        val LOG_NAME = Regex("""vst_host(_[A-Za-z0-9-]+)?\.log|(wineboot|rpcss|turnip|msi_install)\.log""")
    }
}
