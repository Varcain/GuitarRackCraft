/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.varcain.guitarrackcraft.ui.vst

import android.content.Context
import android.system.Os
import android.util.Log
import com.varcain.vsthost.wine.WineSetup
import java.io.File
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.Path

/**
 * Bridge between :app's VST UI and :vsthost_lib's WineSetup. Responsible for:
 *
 *   1. ensureWineRoot()       — global one-time-per-version extraction of
 *                                wine binaries from jniLibs/assets into
 *                                filesDir/wine/ + filesDir/wineprefix/ +
 *                                staging of vst_host.exe variants.
 *   2. ensurePluginPrefix()    — per-imported-VST clone of the base prefix,
 *                                seeded with the same registry/DLL overrides
 *                                vstpoc's HostViewModel applies. Idempotent.
 *
 * Both are blocking, slow (seconds), and must NOT run on the main thread.
 * Callers wrap in Dispatchers.IO.
 */
object VstHostSetup {
    private const val TAG = "VstHostSetup"

    /** Lives in filesDir alongside vstpoc's existing artifacts; one stamp
     *  per imported VST keyed by uuid so we don't re-seed on every rack add. */
    private const val PREFIX_STAMP_DIR = "vst_plugin_prefixes_ready"

    fun ensureWineRoot(context: Context): Boolean {
        return try {
            val setup = WineSetup.ensure(context)
            Log.i(TAG, "wine setup ready: root=${setup.wineRoot.absolutePath}")
            // Stage vst_host.exe variants into filesDir/. vstpoc's
            // HostViewModel.startWineVst then copies from filesDir/ into
            // filesDir/tmp/ at startWineVst time; our WineVstPlugin reads
            // from assetsDir which we point at filesDir.
            for (name in listOf("vst_host.exe", "vst_host_x86.exe", "vst3_host.exe")) {
                runCatching {
                    context.assets.open(name).use { input ->
                        File(context.filesDir, name).outputStream().use { input.copyTo(it) }
                    }
                }
            }
            // Virtual-desktop registry: required before any wine process spawns
            // so winex11.drv sizes the desktop window properly.
            WineSetup.applyVirtualDesktopRegistry(context)
            true
        } catch (t: Throwable) {
            Log.e(TAG, "WineSetup.ensure failed", t)
            false
        }
    }

    /**
     * Make sure a wineprefix exists for the given imported VST. Mirrors
     * vstpoc's HostViewModel:880-953 per-prefix seeding:
     *   - clone base wineprefix → wineprefix_v<uuid>
     *   - seed Win7 version + disable D3D + disable menubuilder
     *   - install DXVK + UIHost stub
     *   - register WinRT activatable classes + Common Controls SxS manifest
     *   - seed Program Files dirs + registry
     *
     * Idempotent — skip on subsequent calls via a stamp file. Sentinel file
     * makes it safe to call from import (where it's blocking with progress UI)
     * and from rack-add (where prefix already exists from import).
     */
    fun ensurePluginPrefix(context: Context, uuid: String): Boolean {
        val basePrefix = File(context.filesDir, "wineprefix")
        if (!basePrefix.exists()) {
            Log.e(TAG, "base wineprefix missing — call ensureWineRoot first")
            return false
        }
        val prefix = File(context.filesDir, "wineprefix_v$uuid")
        val stampDir = File(context.filesDir, PREFIX_STAMP_DIR).apply { mkdirs() }
        val stamp = File(stampDir, "$uuid.ready")
        return try {
            if (!prefix.exists()) {
                Log.i(TAG, "cloning $basePrefix → $prefix")
                copyDirectoryTree(basePrefix, prefix)
            }
            // Idempotent seeders — safe to call every time the user opens the
            // manager. Cheap on the second pass.
            WineSetup.seedWindowsVersion(prefix)
            WineSetup.seedDisableDirect3D(prefix)
            WineSetup.seedDisableMenubuilder(prefix)
            WineSetup.installDxvk(context, prefix)
            WineSetup.seedActivatableClasses(prefix)
            WineSetup.seedCommonControlsManifests(prefix)
            WineSetup.seedProgramFilesDirs(prefix)
            WineSetup.installUiHostStub(context, prefix)
            stamp.writeText(System.currentTimeMillis().toString())
            true
        } catch (t: Throwable) {
            Log.e(TAG, "ensurePluginPrefix($uuid) failed", t)
            false
        }
    }

    /** Public symlink-preserving copy (reused by the installer flow to
     *  clone the base prefix into a one-shot template). */
    fun copyPrefix(src: File, dst: File) = copyDirectoryTree(src, dst)

    /** Idempotent seed application on an EXISTING prefix. Called from the
     *  installer flow's confirmPicks after the template is cloned to the
     *  per-plugin prefix — the seeds (DXVK DLLs, UI host stub, Win7 spoof,
     *  Common Controls SxS manifests) need to land on the per-plugin
     *  prefix the same way ensurePluginPrefix lands them on a fresh
     *  base-cloned prefix. Safe to call on a prefix that already had
     *  these seeds applied; each helper is idempotent. */
    fun applyPluginPrefixSeeds(context: Context, prefix: File) {
        WineSetup.seedWindowsVersion(prefix)
        WineSetup.seedDisableDirect3D(prefix)
        WineSetup.seedDisableMenubuilder(prefix)
        WineSetup.installDxvk(context, prefix)
        WineSetup.seedActivatableClasses(prefix)
        WineSetup.seedCommonControlsManifests(prefix)
        WineSetup.seedProgramFilesDirs(prefix)
        WineSetup.installUiHostStub(context, prefix)
    }

    /** Recursive copy that PRESERVES SYMLINKS — critical for wineprefix
     *  clones because dosdevices/c: and dosdevices/z: are symlinks
     *  (-> ../drive_c, -> /) and wine's path resolution requires them as
     *  symlinks. The naive File.isDirectory() check follows symlinks, so
     *  copying it as a regular tree dereferences c: to drive_c (good) and
     *  z: to / (catastrophic — tries to copy the entire filesystem). */
    private fun copyDirectoryTree(src: File, dst: File) {
        dst.mkdirs()
        src.listFiles()?.forEach { entry ->
            val target = File(dst, entry.name)
            val isSymlink = Files.isSymbolicLink(entry.toPath())
            when {
                isSymlink -> {
                    // Read the symlink target and recreate as a symlink.
                    val link: Path = entry.toPath()
                    val linkTarget = Files.readSymbolicLink(link).toString()
                    target.delete()
                    Os.symlink(linkTarget, target.absolutePath)
                }
                Files.isDirectory(entry.toPath(), LinkOption.NOFOLLOW_LINKS) -> {
                    copyDirectoryTree(entry, target)
                }
                else -> {
                    entry.inputStream().use { input ->
                        target.outputStream().use { output -> input.copyTo(output) }
                    }
                }
            }
        }
    }
}
