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

package com.varcain.guitarrackcraft.ui.recordings

import android.content.Intent
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.LibraryMusic
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import com.varcain.guitarrackcraft.engine.RecordingEntry
import com.varcain.guitarrackcraft.engine.RecordingManager
import com.varcain.guitarrackcraft.engine.hasPreset
import com.varcain.guitarrackcraft.engine.readPresetJson

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun RecordingsScreen(
    onNavigateBack: () -> Unit,
    onPlayRecording: (path: String) -> Unit,
    onLoadRecordingPreset: (json: String) -> Unit = {}
) {
    val context = LocalContext.current
    var recordings by remember { mutableStateOf(RecordingManager.listRecordings(context)) }
    var deleteTarget by remember { mutableStateOf<RecordingEntry?>(null) }

    BackHandler { onNavigateBack() }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Recordings") },
                navigationIcon = {
                    IconButton(onClick = { onNavigateBack() }) {
                        Icon(Icons.Default.ArrowBack, contentDescription = "Back")
                    }
                }
            )
        }
    ) { padding ->
        if (recordings.isEmpty()) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(padding),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    "No recordings yet.\nUse the Record button to capture audio.",
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        } else {
            LazyColumn(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(padding),
                contentPadding = PaddingValues(16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                items(recordings, key = { it.timestamp }) { entry ->
                    RecordingCard(
                        entry = entry,
                        onPlayRaw = { onPlayRecording(entry.rawFile.absolutePath) },
                        onPlayProcessed = { onPlayRecording(entry.processedFile.absolutePath) },
                        onShare = {
                            val authority = "${context.packageName}.fileprovider"
                            val rawUri = FileProvider.getUriForFile(context, authority, entry.rawFile)
                            val procUri = FileProvider.getUriForFile(context, authority, entry.processedFile)
                            val intent = Intent(Intent.ACTION_SEND_MULTIPLE).apply {
                                type = "audio/wav"
                                putParcelableArrayListExtra(Intent.EXTRA_STREAM, arrayListOf(rawUri, procUri))
                                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                            }
                            context.startActivity(Intent.createChooser(intent, "Share Recording"))
                        },
                        onLoadPreset = { json -> onLoadRecordingPreset(json) },
                        onDelete = { deleteTarget = entry }
                    )
                }
            }
        }
    }

    // Delete confirmation dialog
    deleteTarget?.let { entry ->
        AlertDialog(
            onDismissRequest = { deleteTarget = null },
            title = { Text("Delete Recording") },
            text = { Text("Delete recording from ${entry.displayName}? This cannot be undone.") },
            confirmButton = {
                TextButton(onClick = {
                    RecordingManager.deleteRecording(entry)
                    recordings = RecordingManager.listRecordings(context)
                    deleteTarget = null
                }) {
                    Text("Delete", color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { deleteTarget = null }) {
                    Text("Cancel")
                }
            }
        )
    }
}

@Composable
private fun RecordingCard(
    entry: RecordingEntry,
    onPlayRaw: () -> Unit,
    onPlayProcessed: () -> Unit,
    onShare: () -> Unit,
    onLoadPreset: (json: String) -> Unit,
    onDelete: () -> Unit
) {
    val context = LocalContext.current
    var presetMenuExpanded by remember { mutableStateOf(false) }

    Card(
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Text(
                text = entry.displayName,
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.Medium
            )
            Text(
                text = formatDuration(entry.durationSec),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                OutlinedButton(
                    onClick = onPlayRaw,
                    modifier = Modifier.weight(1f),
                    contentPadding = PaddingValues(horizontal = 8.dp, vertical = 4.dp)
                ) {
                    Text("Play Raw", style = MaterialTheme.typography.labelSmall)
                }
                OutlinedButton(
                    onClick = onPlayProcessed,
                    modifier = Modifier.weight(1f),
                    contentPadding = PaddingValues(horizontal = 8.dp, vertical = 4.dp)
                ) {
                    Text("Play Processed", style = MaterialTheme.typography.labelSmall)
                }
                Box {
                    IconButton(
                        onClick = { presetMenuExpanded = true },
                        enabled = entry.hasPreset(),
                        modifier = Modifier.size(36.dp)
                    ) {
                        Icon(
                            Icons.Default.LibraryMusic,
                            contentDescription = "Preset",
                            modifier = Modifier.size(18.dp)
                        )
                    }
                    DropdownMenu(
                        expanded = presetMenuExpanded,
                        onDismissRequest = { presetMenuExpanded = false }
                    ) {
                        DropdownMenuItem(
                            text = { Text("Load Preset") },
                            onClick = {
                                presetMenuExpanded = false
                                entry.readPresetJson()?.let { onLoadPreset(it) }
                            }
                        )
                        DropdownMenuItem(
                            text = { Text("Share Preset") },
                            onClick = {
                                presetMenuExpanded = false
                                entry.readPresetJson()?.let { json ->
                                    val intent = Intent(Intent.ACTION_SEND).apply {
                                        type = "text/plain"
                                        putExtra(Intent.EXTRA_TEXT, json)
                                    }
                                    context.startActivity(Intent.createChooser(intent, "Share Preset"))
                                }
                            }
                        )
                    }
                }
                IconButton(onClick = onShare, modifier = Modifier.size(36.dp)) {
                    Icon(Icons.Default.Share, contentDescription = "Share", modifier = Modifier.size(18.dp))
                }
                IconButton(onClick = onDelete, modifier = Modifier.size(36.dp)) {
                    Icon(
                        Icons.Default.Delete,
                        contentDescription = "Delete",
                        tint = MaterialTheme.colorScheme.error,
                        modifier = Modifier.size(18.dp)
                    )
                }
            }
        }
    }
}

private fun formatDuration(seconds: Double): String {
    val totalSec = seconds.toInt()
    val min = totalSec / 60
    val sec = totalSec % 60
    return "Duration: %d:%02d".format(min, sec)
}
