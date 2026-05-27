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
                installerName = session?.displayName ?: "…",
                onCancel = { vm.cancel() },
            )
        }
        VstInstallerViewModel.State.PICK -> {
            Surface(
                modifier = Modifier.fillMaxSize(),
                color = MaterialTheme.colorScheme.background,
            ) {
                Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {
                    PhaseHeader(state, session?.displayName ?: "…", onCancel = null)
                    Spacer(Modifier.height(12.dp))
                    PickList(
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
                    PhaseHeader(state, session?.displayName ?: "…",
                                onCancel = { vm.cancel() })
                    Spacer(Modifier.height(12.dp))
                    SpinnerBox()
                }
            }
        }
    }
}

/** RUNNING-phase view: edge-to-edge SurfaceView with a small floating
 *  Cancel button overlay. Wine installer wizards typically need every
 *  pixel; the floating button is the only chrome. */
@Composable
private fun RunningOverlay(installerName: String, onCancel: () -> Unit) {
    val displayNumber = VstInstallerViewModel.INSTALLER_DISPLAY_NUMBER
    val screenW = VstInstallerViewModel.INSTALLER_SCREEN_W
    val screenH = VstInstallerViewModel.INSTALLER_SCREEN_H
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
            Icon(Icons.Default.Close, contentDescription = "Cancel installer",
                 tint = Color.White)
        }
    }
}

@Composable
private fun PhaseHeader(
    state: VstInstallerViewModel.State,
    installerName: String,
    onCancel: (() -> Unit)?,
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = "Installing $installerName",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                text = phaseLabel(state),
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
    discovered: List<VstInstallerViewModel.DiscoveredPlugin>,
    onConfirm: (List<VstInstallerViewModel.DiscoveredPlugin>) -> Unit,
    onCancel: () -> Unit,
) {
    // Default to all checked — usually the user wants every VST the
    // installer dropped. Easy to uncheck the dud copies (32-bit, bundled
    // demo plugins, etc.) before confirming.
    val picked = remember(discovered) {
        mutableStateMapOf<String, Boolean>().also {
            discovered.forEach { p -> it[p.absPath] = true }
        }
    }
    Column(modifier = Modifier.fillMaxSize()) {
        Text(
            "${discovered.size} VST plugin(s) found in the installed prefix. " +
            "Uncheck any you don't want imported.",
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
                            "${if (p.isVst3) "VST3" else "VST2"} · " +
                            "${if (p.is64Bit) "x64" else "x86"} · ${p.relToPrefix}",
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

private fun phaseLabel(s: VstInstallerViewModel.State): String = when (s) {
    VstInstallerViewModel.State.IDLE -> ""
    VstInstallerViewModel.State.PREPARING -> "Preparing wineprefix…"
    VstInstallerViewModel.State.RUNNING -> "Wizard running — interact below"
    VstInstallerViewModel.State.DRAINING -> "Finalising install…"
    VstInstallerViewModel.State.DISCOVERING -> "Scanning for installed VSTs…"
    VstInstallerViewModel.State.PICK -> "Pick which plugins to import"
}
