/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.varcain.guitarrackcraft.ui.vst

import android.app.Activity
import android.content.pm.ActivityInfo
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.varcain.vsthost.NativeBridge
import com.varcain.vsthost.ui.PluginSurface

/**
 * Full-screen overlay shown for the install-from-.exe flow. Rendered
 * inside VstManagerScreen when the installer view model isn't IDLE.
 *
 * Phases:
 *  - PREPARING: spinner + "Cloning wineprefix"
 *  - RUNNING:   embedded EditorSurfaceView on X11 display 99 — the wine
 *               installer wizard renders here. Touch + key events flow
 *               via vsthost_lib's standard injection JNI.
 *  - DRAINING:  spinner + "Finalising"
 *  - DISCOVERING: spinner + "Scanning prefix"
 *  - PICK:      checkbox list of discovered VSTs + Confirm / Cancel
 */
@Composable
fun VstInstallerScreen(vm: VstInstallerViewModel = viewModel()) {
    val state by vm.state.collectAsState()
    val session by vm.session.collectAsState()
    val discovered by vm.discovered.collectAsState()
    val mode = session?.mode ?: VstInstallerViewModel.Mode.INSTALL

    // Force landscape while the installer overlay is up — installer wizards
    // are 4:3 or 16:9 by convention and look squished/clipped in portrait.
    // Restore default orientation when we exit back to IDLE.
    val activity = LocalContext.current as? Activity
    DisposableEffect(Unit) {
        val previous = activity?.requestedOrientation ?: ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
        onDispose { activity?.requestedOrientation = previous }
    }

    when (state) {
        VstInstallerViewModel.State.RUNNING -> {
            RunningOverlay(
                mode = mode,
                onCancel = { vm.cancel() },
            )
        }
        VstInstallerViewModel.State.PICK -> {
            Surface(
                modifier = Modifier.fillMaxSize(),
                color = MaterialTheme.colorScheme.background,
            ) {
                Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {
                    PhaseHeader(state, mode, session?.displayName ?: "…", onCancel = null)
                    Spacer(Modifier.height(12.dp))
                    PickList(
                        mode = mode,
                        discovered = discovered,
                        onConfirm = { picks -> vm.confirmPicks(picks) },
                        onCancel = { vm.cancel() },
                    )
                }
            }
        }
        else -> {
            // PREPARING / DRAINING / DISCOVERING / IDLE — all spinner-ish.
            Surface(
                modifier = Modifier.fillMaxSize(),
                color = MaterialTheme.colorScheme.background,
            ) {
                Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {
                    PhaseHeader(state, mode, session?.displayName ?: "…",
                                onCancel = { vm.cancel() })
                    Spacer(Modifier.height(12.dp))
                    SpinnerBox()
                }
            }
        }
    }
}

/** RUNNING-phase view: edge-to-edge SurfaceView with a small floating
 *  Close button overlay. Wine installer wizards typically need every
 *  pixel; the floating button is the only chrome. In LAUNCH mode the
 *  same button means "close the manager and re-scan the prefix for new
 *  plugins"; in INSTALL mode it means "cancel the install and discard
 *  the template". */
@Composable
private fun RunningOverlay(mode: VstInstallerViewModel.Mode, onCancel: () -> Unit) {
    val displayNumber = VstInstallerViewModel.INSTALLER_DISPLAY_NUMBER
    val screenW = VstInstallerViewModel.INSTALLER_SCREEN_W
    val screenH = VstInstallerViewModel.INSTALLER_SCREEN_H
    val closeDescription = if (mode == VstInstallerViewModel.Mode.LAUNCH)
        "Close manager (scans for new plugins)"
    else
        "Cancel installer"
    // X server is started by the view model BEFORE forking wine — see the
    // pre-start in installFromExe(). We re-call setPluginSize here as a
    // belt-and-braces guard against slot-promotion shrinking the
    // framebuffer when wine's wizard window (~500x350) lands as a child
    // of the root with no parent slot established yet.
    DisposableEffect(displayNumber) {
        NativeBridge.nativeSetX11PluginSize(displayNumber, screenW, screenH)
        onDispose { /* server stays alive across phase changes */ }
    }
    Box(modifier = Modifier.fillMaxSize().background(Color.Black)) {
        PluginSurface(
            displayNumber = displayNumber,
            pluginWidth = screenW,
            pluginHeight = screenH,
            modifier = Modifier.fillMaxSize(),
            // Installer display doesn't outlive this composition — destroy
            // on dispose so the next install starts from a clean slot.
            destroyOnDispose = true,
        )
        // Floating Cancel: top-right, 50% black backdrop for visibility
        // over any wine background. Mirrors the LV2/VST fullscreen exit.
        IconButton(
            onClick = onCancel,
            modifier = Modifier
                .align(Alignment.TopEnd)
                .padding(12.dp)
                .background(Color.Black.copy(alpha = 0.5f), CircleShape),
        ) {
            Icon(Icons.Default.Close, contentDescription = closeDescription,
                 tint = Color.White)
        }
    }
}

@Composable
private fun PhaseHeader(
    state: VstInstallerViewModel.State,
    mode: VstInstallerViewModel.Mode,
    installerName: String,
    onCancel: (() -> Unit)?,
) {
    val verb = if (mode == VstInstallerViewModel.Mode.LAUNCH) "Running" else "Installing"
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = "$verb $installerName",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                text = phaseLabel(state, mode),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        if (onCancel != null) {
            OutlinedButton(onClick = onCancel) {
                Icon(Icons.Default.Close, contentDescription = null,
                     modifier = Modifier.size(16.dp))
                Spacer(Modifier.width(4.dp))
                Text("Cancel")
            }
        }
    }
}

@Composable
private fun SpinnerBox() {
    Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            CircularProgressIndicator()
            Spacer(Modifier.height(12.dp))
        }
    }
}

@Composable
private fun PickList(
    mode: VstInstallerViewModel.Mode,
    discovered: List<VstInstallerViewModel.DiscoveredPlugin>,
    onConfirm: (List<VstInstallerViewModel.DiscoveredPlugin>) -> Unit,
    onCancel: () -> Unit,
) {
    // Default: VSTs checked (usually the user wants every plugin the
    // installer dropped), executables UNchecked (most installs only have
    // one actual manager exe worth registering — make the user opt in
    // rather than auto-register helper exes that slipped past the filter).
    val picked = remember(discovered) {
        mutableStateMapOf<String, Boolean>().also {
            discovered.forEach { p -> it[p.absPath] = !p.isExecutable }
        }
    }
    val vstCount = discovered.count { !it.isExecutable }
    val exeCount = discovered.count { it.isExecutable }
    Column(modifier = Modifier.fillMaxSize()) {
        Text(
            buildString {
                if (mode == VstInstallerViewModel.Mode.LAUNCH) {
                    append("$vstCount newly-installed VST plugin(s) found. " +
                           "Uncheck any you don't want imported.")
                } else {
                    append("$vstCount VST plugin(s)")
                    if (exeCount > 0) append(" + $exeCount manager exe(s)")
                    append(" found in the installed prefix. Uncheck any you don't want imported.")
                }
            },
            style = MaterialTheme.typography.bodyMedium,
        )
        Spacer(Modifier.height(8.dp))
        LazyColumn(modifier = Modifier.weight(1f)) {
            items(discovered, key = { it.absPath }) { p ->
                Row(
                    modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Checkbox(
                        checked = picked[p.absPath] ?: false,
                        onCheckedChange = { picked[p.absPath] = it },
                    )
                    Column(modifier = Modifier.weight(1f)) {
                        Text(p.displayName, style = MaterialTheme.typography.bodyLarge,
                             maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(
                            buildString {
                                append(when (p.kind) {
                                    VstInstallerViewModel.Kind.VST3 -> "VST3"
                                    VstInstallerViewModel.Kind.VST2 -> "VST2"
                                    VstInstallerViewModel.Kind.EXECUTABLE -> "Executable"
                                })
                                append(" · ")
                                append(if (p.is64Bit) "x64" else "x86")
                                if (p.isExecutable && p.sizeBytes > 0) {
                                    append(" · ")
                                    append(formatBytes(p.sizeBytes))
                                }
                                append(" · ")
                                append(p.relToPrefix)
                            },
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 1, overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
                Divider()
            }
        }
        Spacer(Modifier.height(12.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            OutlinedButton(onClick = onCancel, modifier = Modifier.weight(1f)) {
                Text("Discard all")
            }
            Button(
                onClick = {
                    val picks = discovered.filter { picked[it.absPath] == true }
                    onConfirm(picks)
                },
                modifier = Modifier.weight(1f),
                enabled = discovered.any { picked[it.absPath] == true },
            ) {
                Text("Import selected")
            }
        }
    }
}

private fun formatBytes(bytes: Long): String = when {
    bytes >= 1_073_741_824L -> "%.1f GB".format(bytes / 1_073_741_824.0)
    bytes >= 1_048_576L -> "%.1f MB".format(bytes / 1_048_576.0)
    bytes >= 1024L -> "%.0f KB".format(bytes / 1024.0)
    else -> "$bytes B"
}

private fun phaseLabel(
    s: VstInstallerViewModel.State,
    mode: VstInstallerViewModel.Mode,
): String = when (s) {
    VstInstallerViewModel.State.IDLE -> ""
    VstInstallerViewModel.State.PREPARING ->
        if (mode == VstInstallerViewModel.Mode.LAUNCH)
            "Snapshotting prefix…"
        else
            "Preparing wineprefix…"
    VstInstallerViewModel.State.RUNNING ->
        if (mode == VstInstallerViewModel.Mode.LAUNCH)
            "Manager running — close (X) to scan for new plugins"
        else
            "Wizard running — interact below"
    VstInstallerViewModel.State.DRAINING ->
        if (mode == VstInstallerViewModel.Mode.LAUNCH) "Finalising…"
        else "Finalising install…"
    VstInstallerViewModel.State.DISCOVERING ->
        if (mode == VstInstallerViewModel.Mode.LAUNCH)
            "Scanning for newly-installed VSTs…"
        else
            "Scanning for installed VSTs…"
    VstInstallerViewModel.State.PICK -> "Pick which plugins to import"
}
