/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

package com.varcain.guitarrackcraft.ui.settings

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import com.varcain.guitarrackcraft.engine.AudioDeviceOption
import com.varcain.guitarrackcraft.engine.AudioEngine
import com.varcain.guitarrackcraft.engine.AudioSettingsManager
import com.varcain.guitarrackcraft.ui.rack.RackViewModel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AudioSettingsScreen(
    viewModel: RackViewModel,
    onNavigateBack: () -> Unit
) {
    val context = LocalContext.current

    val inputDevices = remember { AudioSettingsManager.getInputDevices(context) }
    val outputDevices = remember { AudioSettingsManager.getOutputDevices(context) }

    var selectedInputId by remember { mutableIntStateOf(AudioSettingsManager.getInputDeviceId(context)) }
    var selectedOutputId by remember { mutableIntStateOf(AudioSettingsManager.getOutputDeviceId(context)) }
    var selectedBufferSize by remember { mutableIntStateOf(AudioSettingsManager.getBufferSize(context)) }
    var refreshKey by remember { mutableIntStateOf(0) }

    BackHandler { onNavigateBack() }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Audio Settings") },
                navigationIcon = {
                    IconButton(onClick = { onNavigateBack() }) {
                        Icon(Icons.Default.ArrowBack, contentDescription = "Back")
                    }
                }
            )
        }
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 16.dp, vertical = 8.dp)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(20.dp)
        ) {
            // Input device selector
            DeviceDropdown(
                label = "Input Device",
                devices = inputDevices,
                selectedId = selectedInputId,
                onSelected = { id ->
                    selectedInputId = id
                    AudioSettingsManager.setInputDeviceId(context, id)
                    viewModel.restartEngine(context)
                    refreshKey++
                }
            )

            // Output device selector
            DeviceDropdown(
                label = "Output Device",
                devices = outputDevices,
                selectedId = selectedOutputId,
                onSelected = { id ->
                    selectedOutputId = id
                    AudioSettingsManager.setOutputDeviceId(context, id)
                    viewModel.restartEngine(context)
                    refreshKey++
                }
            )

            // Buffer size selector
            BufferSizeDropdown(
                selectedSize = selectedBufferSize,
                onSelected = { size ->
                    selectedBufferSize = size
                    AudioSettingsManager.setBufferSize(context, size)
                    viewModel.restartEngine(context)
                    refreshKey++
                }
            )

            Text(
                text = "Lower buffer sizes reduce latency but increase CPU usage. Use Auto unless you experience issues.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            // Current engine info + low-latency checklist
            val isRunning by viewModel.isEngineRunning.collectAsState()
            if (isRunning) {
                val sampleRate = remember(refreshKey) { AudioEngine.getSampleRate() }
                val bufferFrames = remember(refreshKey) { AudioEngine.getBufferFrameCount() }
                val streamInfo = remember(refreshKey) { AudioEngine.getStreamInfo() }

                Divider()
                Text(
                    text = "Current Session",
                    style = MaterialTheme.typography.labelLarge,
                    modifier = Modifier.padding(bottom = 4.dp)
                )
                InfoRow("Sample Rate", "%.0f Hz".format(sampleRate))
                InfoRow("Buffer Size", "$bufferFrames frames")
                InfoRow("Burst Size", "${streamInfo.framesPerBurst} frames")
                InfoRow("Audio Format", "32-bit Float")

                Spacer(modifier = Modifier.height(4.dp))
                Divider()
                Text(
                    text = "Low-Latency Checklist",
                    style = MaterialTheme.typography.labelLarge,
                    modifier = Modifier.padding(bottom = 4.dp)
                )
                Text(
                    text = "Based on developer.android.com/games/sdk/oboe/low-latency-audio",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(bottom = 8.dp)
                )

                ChecklistItem("Oboe API", true)
                ChecklistItem("AAudio backend", streamInfo.isAAudio, "OpenSL ES")
                ChecklistItem("Performance: Low Latency", streamInfo.outputLowLatency)
                ChecklistItem("Sharing: Exclusive (output)", streamInfo.outputExclusive, "Shared")
                ChecklistItem("Sharing: Exclusive (input)", streamInfo.inputExclusive, "Shared")
                ChecklistItem("Sample rate: 48000 Hz", sampleRate.toInt() == 48000, "%.0f Hz".format(sampleRate))
                ChecklistItem("Data callback", streamInfo.outputCallback)
                ChecklistItem("MMAP buffer", streamInfo.outputMMap)
            }
        }
    }
}

@Composable
private fun InfoRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium
        )
    }
}

@Composable
private fun ChecklistItem(label: String, enabled: Boolean, disabledDetail: String? = null) {
    val green = Color(0xFF4CAF50)
    val red = Color(0xFFF44336)
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 3.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = if (enabled) "\u2713" else "\u2717",
            color = if (enabled) green else red,
            fontWeight = FontWeight.Bold,
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.width(24.dp)
        )
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.weight(1f)
        )
        if (!enabled && disabledDetail != null) {
            Text(
                text = disabledDetail,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DeviceDropdown(
    label: String,
    devices: List<AudioDeviceOption>,
    selectedId: Int,
    onSelected: (Int) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    val selectedDevice = devices.find { it.id == selectedId } ?: devices.firstOrNull()

    Column {
        Text(
            text = label,
            style = MaterialTheme.typography.labelLarge,
            modifier = Modifier.padding(bottom = 4.dp)
        )
        ExposedDropdownMenuBox(
            expanded = expanded,
            onExpandedChange = { expanded = it }
        ) {
            OutlinedTextField(
                value = selectedDevice?.name ?: "Default",
                onValueChange = {},
                readOnly = true,
                modifier = Modifier
                    .fillMaxWidth()
                    .menuAnchor(),
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) }
            )
            ExposedDropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false }
            ) {
                devices.forEach { device ->
                    DropdownMenuItem(
                        text = { Text(device.name) },
                        onClick = {
                            onSelected(device.id)
                            expanded = false
                        }
                    )
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun BufferSizeDropdown(
    selectedSize: Int,
    onSelected: (Int) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    val options = AudioSettingsManager.BUFFER_SIZE_OPTIONS
    val selectedLabel = options.find { it.first == selectedSize }?.second ?: "Auto"

    Column {
        Text(
            text = "Buffer Size",
            style = MaterialTheme.typography.labelLarge,
            modifier = Modifier.padding(bottom = 4.dp)
        )
        ExposedDropdownMenuBox(
            expanded = expanded,
            onExpandedChange = { expanded = it }
        ) {
            OutlinedTextField(
                value = selectedLabel,
                onValueChange = {},
                readOnly = true,
                modifier = Modifier
                    .fillMaxWidth()
                    .menuAnchor(),
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) }
            )
            ExposedDropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false }
            ) {
                options.forEach { (size, label) ->
                    DropdownMenuItem(
                        text = { Text(label) },
                        onClick = {
                            onSelected(size)
                            expanded = false
                        }
                    )
                }
            }
        }
    }
}
