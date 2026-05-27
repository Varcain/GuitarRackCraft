/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.varcain.guitarrackcraft.ui.vst

import android.content.Context
import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.OpenInNew
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.varcain.guitarrackcraft.engine.NativeEngine
import com.varcain.guitarrackcraft.engine.RackManager
import androidx.lifecycle.viewmodel.compose.viewModel
import com.varcain.vsthost.NativeBridge
import com.varcain.vsthost.PeFlag
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.UUID

/** One imported VST as persisted in filesDir/vst_plugins/registry.json. */
data class VstRegistryEntry(
    val uuid: String,
    val displayName: String,
    val format: String,   // "VST2" or "VST3"
    val dllPath: String,
    val is64Bit: Boolean,
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun VstManagerScreen(onNavigateBack: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var entries by remember { mutableStateOf(VstRegistry.read(context)) }
    var importingName by remember { mutableStateOf<String?>(null) }

    var setupInProgress by remember { mutableStateOf(false) }

    // Installer flow: full-screen overlay while installerVm.state != IDLE.
    val installerVm: VstInstallerViewModel = viewModel()
    val installerState by installerVm.state.collectAsState()
    val installerError by installerVm.errorMessage.collectAsState()
    LaunchedEffect(installerError) {
        installerError?.let {
            Toast.makeText(context, it, Toast.LENGTH_LONG).show()
            installerVm.consumeError()
            // Re-read in case the installer's PICK phase wrote new entries.
            entries = VstRegistry.read(context)
        }
    }
    LaunchedEffect(installerState) {
        // PICK→IDLE transition writes the picked plugins; refresh.
        if (installerState == VstInstallerViewModel.State.IDLE) {
            entries = VstRegistry.read(context)
        }
    }
    if (installerState != VstInstallerViewModel.State.IDLE) {
        VstInstallerScreen(installerVm)
        return
    }

    val exePickerLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            setupInProgress = true
            val staged = withContext(Dispatchers.IO) {
                if (!VstHostSetup.ensureWineRoot(context)) return@withContext null
                stageInstaller(context, uri)
            }
            setupInProgress = false
            if (staged == null) {
                Toast.makeText(context, "Couldn't stage the installer file", Toast.LENGTH_LONG).show()
                return@launch
            }
            installerVm.installFromExe(staged.absolutePath, staged.nameWithoutExtension)
        }
    }

    val pickerLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            setupInProgress = true
            val result = withContext(Dispatchers.IO) {
                // First-import path: extract wine binaries if not done. Slow
                // (~5-10s) the first time, instant on subsequent imports.
                if (!VstHostSetup.ensureWineRoot(context)) {
                    return@withContext ImportResult.Err("Wine runtime setup failed (see logcat)")
                }
                val r = VstRegistry.importFrom(context, uri)
                if (r is ImportResult.Ok) {
                    // Per-plugin prefix needs to exist BEFORE the audio engine
                    // tries to spawn wine. Do it now (still in IO context).
                    VstHostSetup.ensurePluginPrefix(context, r.uuid)
                }
                r
            }
            setupInProgress = false
            when (result) {
                is ImportResult.Ok -> {
                    entries = VstRegistry.read(context)
                    importingName = result.displayName
                    // Trigger native PluginRegistry refresh so the new VST
                    // appears in the browser without restarting the engine.
                    runCatching { NativeEngine.getInstance().nativeRefreshPluginRegistry() }
                    Toast.makeText(
                        context,
                        "Imported ${result.displayName} — available in Add Plugin",
                        Toast.LENGTH_LONG
                    ).show()
                }
                is ImportResult.Err ->
                    Toast.makeText(context, result.reason, Toast.LENGTH_LONG).show()
            }
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Manage VST") },
                navigationIcon = {
                    IconButton(onClick = onNavigateBack) {
                        Icon(Icons.Default.ArrowBack, contentDescription = "Back")
                    }
                }
            )
        }
    ) { padding ->
        Column(
            modifier = Modifier.padding(padding).fillMaxSize().padding(16.dp)
        ) {
            Text(
                "Imported VSTs",
                style = MaterialTheme.typography.titleMedium
            )
            Spacer(Modifier.height(8.dp))
            if (entries.isEmpty()) {
                Text(
                    "No VSTs imported yet. Tap Import below to pick a .dll or .vst3 file.",
                    style = MaterialTheme.typography.bodyMedium
                )
            } else {
                LazyColumn(modifier = Modifier.weight(1f)) {
                    items(entries, key = { it.uuid }) { e ->
                        VstRow(
                            entry = e,
                            onOpenEditor = {
                                openVstEditor(context, e.uuid)
                            },
                            onRemove = {
                                scope.launch(Dispatchers.IO) {
                                    VstRegistry.remove(context, e.uuid)
                                    val updated = VstRegistry.read(context)
                                    withContext(Dispatchers.Main) {
                                        entries = updated
                                        runCatching {
                                            NativeEngine.getInstance().nativeRefreshPluginRegistry()
                                        }
                                    }
                                }
                            }
                        )
                        Divider()
                    }
                }
            }
            Spacer(Modifier.height(16.dp))
            Button(
                onClick = { pickerLauncher.launch(arrayOf("*/*")) },
                enabled = !setupInProgress,
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(if (setupInProgress) "Setting up wine…" else "Import VST…")
            }
            Spacer(Modifier.height(8.dp))
            OutlinedButton(
                onClick = { exePickerLauncher.launch(arrayOf("*/*")) },
                enabled = !setupInProgress,
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("Install from .exe…")
            }
            if (setupInProgress) {
                Spacer(Modifier.height(8.dp))
                LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                Text(
                    "First import takes ~10–30s to extract wine + FEX binaries.",
                    style = MaterialTheme.typography.bodySmall
                )
            } else {
                Spacer(Modifier.height(8.dp))
                Text(
                    "Imported plugins appear under author \"Varcain\" in the Plugin " +
                    "Browser after the audio engine restarts.",
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }
    }
}

@Composable
private fun VstRow(
    entry: VstRegistryEntry,
    onOpenEditor: () -> Unit,
    onRemove: () -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(entry.displayName, style = MaterialTheme.typography.bodyLarge,
                 maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text("${entry.format} · ${if (entry.is64Bit) "x64" else "x86"}",
                 style = MaterialTheme.typography.bodySmall)
        }
        IconButton(onClick = onOpenEditor) {
            Icon(Icons.Default.OpenInNew, contentDescription = "Open ${entry.displayName} editor")
        }
        IconButton(onClick = onRemove) {
            Icon(Icons.Default.Delete, contentDescription = "Remove ${entry.displayName}")
        }
    }
}

/** Copy the picked .exe URI into a stable on-disk path that wine can read.
 *  Wine resolves Z:\ to / so any absolute path works; we keep installers
 *  in filesDir/installers/<uuid>.exe to avoid littering and to give the
 *  installer view-model something to delete on cleanup. */
private fun stageInstaller(context: Context, uri: Uri): File? {
    val cr = context.contentResolver
    val displayName = cr.query(uri, null, null, null, null)?.use { c ->
        val idx = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
        if (c.moveToFirst() && idx >= 0) c.getString(idx) else "installer.exe"
    } ?: "installer.exe"
    val safeStem = displayName.removeSuffix(".exe").take(64).ifEmpty { "installer" }
    val dir = File(context.filesDir, "installers").apply { mkdirs() }
    val target = File(dir, "${UUID.randomUUID().toString().take(8)}-${safeStem}.exe")
    return runCatching {
        cr.openInputStream(uri)?.use { input ->
            target.outputStream().use { input.copyTo(it) }
        } ?: return null
        target
    }.getOrNull()
}

/** Find the rack position of the VST with [uuid] (if added) and launch
 *  VstEditorActivity bound to its wine display. Otherwise toast a hint. */
private fun openVstEditor(context: Context, uuid: String) {
    val rackPlugins = runCatching { RackManager.getRackPlugins() }.getOrNull().orEmpty()
    val expectedId = "VST2:$uuid"
    val position = rackPlugins.indexOfFirst { it.id == expectedId || it.id.endsWith(":$uuid") }
    if (position < 0) {
        Toast.makeText(
            context,
            "Add this plugin to the rack first, then open its editor.",
            Toast.LENGTH_LONG,
        ).show()
        return
    }
    val display = NativeEngine.getInstance().nativeGetRackPluginX11Display(position)
    if (display < 0) {
        Toast.makeText(
            context,
            "Plugin is in the rack but hasn't allocated a display yet — try again.",
            Toast.LENGTH_LONG,
        ).show()
        return
    }
    context.startActivity(VstEditorActivity.intent(context, display, position))
}

/** Filesystem + JSON I/O for the VST registry. Schema matches what
 *  vsthost_lib's C++ VstFactory::loadRegistry() expects. */
object VstRegistry {
    private const val DIR_NAME = "vst_plugins"
    private const val REGISTRY_FILE = "registry.json"

    fun registryFile(context: Context): File =
        File(context.filesDir, "$DIR_NAME/$REGISTRY_FILE")

    fun pluginsDir(context: Context): File =
        File(context.filesDir, DIR_NAME).apply { mkdirs() }

    fun read(context: Context): List<VstRegistryEntry> {
        val f = registryFile(context)
        if (!f.exists()) return emptyList()
        val body = f.readText()
        val arrStart = body.indexOf("\"plugins\"").takeIf { it >= 0 } ?: return emptyList()
        val arrOpen = body.indexOf('[', arrStart).takeIf { it >= 0 } ?: return emptyList()
        val entries = mutableListOf<VstRegistryEntry>()
        var i = arrOpen + 1
        while (i < body.length) {
            val obj = body.indexOf('{', i).takeIf { it >= 0 } ?: break
            val objEnd = body.indexOf('}', obj).takeIf { it >= 0 } ?: break
            val o = body.substring(obj, objEnd + 1)
            val uuid = extractString(o, "uuid")
            val name = extractString(o, "displayName")
            val fmt  = extractString(o, "format").ifEmpty { "VST2" }
            val dll  = extractString(o, "dllPath")
            val x64  = extractBool(o, "is64Bit", true)
            if (uuid.isNotEmpty() && dll.isNotEmpty()) {
                entries.add(VstRegistryEntry(uuid, name, fmt, dll, x64))
            }
            i = objEnd + 1
        }
        return entries
    }

    fun write(context: Context, entries: List<VstRegistryEntry>) {
        val f = registryFile(context).also { it.parentFile?.mkdirs() }
        val sb = StringBuilder("{\n  \"plugins\": [\n")
        entries.forEachIndexed { idx, e ->
            sb.append("    {")
            sb.append("\"uuid\":\"${esc(e.uuid)}\",")
            sb.append("\"displayName\":\"${esc(e.displayName)}\",")
            sb.append("\"format\":\"${esc(e.format)}\",")
            sb.append("\"dllPath\":\"${esc(e.dllPath)}\",")
            sb.append("\"is64Bit\":${e.is64Bit}")
            sb.append("}")
            if (idx < entries.lastIndex) sb.append(",")
            sb.append("\n")
        }
        sb.append("  ]\n}\n")
        f.writeText(sb.toString())
    }

    fun importFrom(context: Context, uri: Uri): ImportResult {
        val cr = context.contentResolver
        val displayName = cr.query(uri, null, null, null, null)?.use { c ->
            val nameIdx = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
            if (c.moveToFirst() && nameIdx >= 0) c.getString(nameIdx) else "plugin.dll"
        } ?: "plugin.dll"

        val uuid = UUID.randomUUID().toString()
        val pluginDir = File(pluginsDir(context), uuid).apply { mkdirs() }
        // Inspect first to learn VST2 vs VST3, then save with the right extension.
        // The launcher (WineHostProcess.cpp) auto-switches between vst_host.exe
        // and vst3_host.exe based on whether the first plugin path ends in
        // ".vst3" — so the extension is load-bearing, not cosmetic.
        val tmpFile = File(pluginDir, "plugin.tmp")
        cr.openInputStream(uri)?.use { input ->
            tmpFile.outputStream().use { input.copyTo(it) }
        } ?: return ImportResult.Err("Could not open picked file")

        val flags = NativeBridge.nativeInspectPluginExports(tmpFile.absolutePath)
        val isValidPe = (flags and PeFlag.VALID) != 0 && (flags and PeFlag.IS_DLL) != 0
        val isVst2    = (flags and PeFlag.HAS_VSTPLUGINMAIN) != 0
        val isVst3    = (flags and PeFlag.HAS_VST3_FACTORY) != 0
        val is64Bit   = (flags and PeFlag.IS_64) != 0

        if (!isValidPe) {
            tmpFile.delete(); pluginDir.delete()
            return ImportResult.Err("Not a valid Windows PE file")
        }
        if (!isVst2 && !isVst3) {
            tmpFile.delete(); pluginDir.delete()
            return ImportResult.Err("File is a DLL but exports no VST entry point")
        }

        // VST3 plugins are stored with .vst3 extension so vst3_host.exe gets
        // selected by the launcher. VST2 keeps .dll.
        val targetFile = File(pluginDir, if (isVst3) "plugin.vst3" else "plugin.dll")
        tmpFile.renameTo(targetFile)

        val stem = displayName.removeSuffix(".dll").removeSuffix(".vst3")
        val entry = VstRegistryEntry(
            uuid = uuid,
            displayName = stem,
            format = if (isVst3) "VST3" else "VST2",
            dllPath = targetFile.absolutePath,
            is64Bit = is64Bit,
        )
        val all = read(context).toMutableList().also { it.add(entry) }
        write(context, all)
        return ImportResult.Ok(uuid, stem)
    }

    fun remove(context: Context, uuid: String) {
        val all = read(context).filter { it.uuid != uuid }
        write(context, all)
        File(pluginsDir(context), uuid).deleteRecursively()
    }

    private fun esc(s: String): String =
        s.replace("\\", "\\\\").replace("\"", "\\\"")

    private fun extractString(s: String, key: String): String {
        val needle = "\"$key\""
        var p = s.indexOf(needle); if (p < 0) return ""
        p = s.indexOf(':', p);     if (p < 0) return ""
        p = s.indexOf('"', p);     if (p < 0) return ""
        ++p
        val sb = StringBuilder()
        while (p < s.length && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.length) { sb.append(s[p + 1]); p += 2 }
            else                                   { sb.append(s[p]);     ++p    }
        }
        return sb.toString()
    }

    private fun extractBool(s: String, key: String, def: Boolean): Boolean {
        val needle = "\"$key\""
        var p = s.indexOf(needle); if (p < 0) return def
        p = s.indexOf(':', p);     if (p < 0) return def
        ++p
        while (p < s.length && (s[p] == ' ' || s[p] == '\t')) ++p
        return when {
            s.startsWith("true",  p) -> true
            s.startsWith("false", p) -> false
            else                     -> def
        }
    }
}

sealed class ImportResult {
    data class Ok(val uuid: String, val displayName: String) : ImportResult()
    data class Err(val reason: String)                       : ImportResult()
}
