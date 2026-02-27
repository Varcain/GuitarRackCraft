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

package com.varcain.guitarrackcraft.ui.rack

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectDragGesturesAfterLongPress
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.zIndex
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Album
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.FavoriteBorder
import androidx.compose.material.icons.filled.FiberManualRecord
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Cloud
import androidx.compose.material.icons.filled.Dashboard
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.DesktopWindows
import androidx.compose.material.icons.filled.Fullscreen
import androidx.compose.material.icons.filled.LibraryMusic
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.MusicNote
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.Repeat
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material.icons.filled.StopCircle
import androidx.compose.material.icons.filled.SwapHoriz
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.*
import androidx.compose.material3.surfaceColorAtElevation
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.platform.LocalView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import android.content.Intent
import android.util.Log
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.layout.boundsInWindow
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.lifecycle.viewmodel.compose.viewModel
import com.varcain.guitarrackcraft.engine.AudioSettingsManager
import com.varcain.guitarrackcraft.engine.RackManager
import com.varcain.guitarrackcraft.engine.X11Bridge
import com.varcain.guitarrackcraft.engine.PluginInfo
import com.varcain.guitarrackcraft.engine.UiType
import com.varcain.guitarrackcraft.ui.modgui.InlineModguiView
import com.varcain.guitarrackcraft.ui.x11.PluginX11UiView
import com.varcain.guitarrackcraft.ui.x11.X11DisplayManager
import android.net.Uri
import com.varcain.guitarrackcraft.BuildConfig
import com.varcain.guitarrackcraft.engine.RecordingEntry
import com.varcain.guitarrackcraft.engine.RecordingManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.key
import androidx.compose.runtime.rememberCoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.toMutableStateList

/** Item positions for drag-drop: (top in viewport px, height px) per plugin index. */
private class ScrollableDragDropState(
    private val itemPositions: () -> List<Pair<Int, Int>>,
    private val onMove: (Int, Int) -> Unit
) {
    var draggedIndex by mutableStateOf<Int?>(null)
        private set
    var draggedOffset by mutableFloatStateOf(0f)
        private set
    private var draggedInitialTop = 0
    private var originalIndex = -1

    fun onDragStart(viewportY: Float) {
        val positions = itemPositions()
        val y = viewportY.toInt()
        val idx = positions.indexOfFirst { (top, h) -> y in top until (top + h) }
        if (idx < 0) return
        draggedIndex = idx
        originalIndex = idx
        val (top, height) = positions[idx]
        draggedInitialTop = top + height / 2
        draggedOffset = 0f
    }

    fun onDrag(change: Float) {
        draggedOffset += change
        val dragged = draggedIndex ?: return
        val positions = itemPositions()
        if (dragged >= positions.size) return
        val (draggedTop, draggedHeight) = positions[dragged]
        val offsetPx = draggedOffset.toInt()
        // Use leading edge (top when moving up, bottom when moving down) so tall items need minimal drag
        val draggedTopEdge = draggedTop + offsetPx
        val draggedBottomEdge = draggedTop + draggedHeight + offsetPx

        val below = (dragged + 1 until positions.size).firstOrNull()
        val above = (dragged - 1 downTo 0).firstOrNull()
        val targetIndex = when {
            below != null && draggedBottomEdge > positions[below].first -> below
            above != null && draggedTopEdge < positions[above].first + positions[above].second -> above
            else -> null
        }
        if (targetIndex != null && targetIndex != dragged) {
            onMove(dragged, targetIndex)
            val (targetTop, targetHeight) = positions[targetIndex]
            val shift = if (targetIndex > dragged) {
                (targetTop + targetHeight) - (draggedTop + draggedHeight)
            } else {
                targetTop - draggedTop
            }
            draggedInitialTop += shift
            draggedOffset -= shift
            draggedIndex = targetIndex
        }
    }

    fun onDragEnd(): Pair<Int, Int>? {
        val result = draggedIndex?.let { current ->
            if (originalIndex != current) Pair(originalIndex, current) else null
        }
        draggedIndex = null
        draggedOffset = 0f
        originalIndex = -1
        return result
    }

    fun onDragCancel() {
        draggedIndex = null
        draggedOffset = 0f
        originalIndex = -1
    }
}

@Composable
private fun rememberScrollableDragDropState(
    itemPositions: () -> List<Pair<Int, Int>>,
    onMove: (Int, Int) -> Unit
): ScrollableDragDropState {
    return remember(itemPositions, onMove) { ScrollableDragDropState(itemPositions, onMove) }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun RackScreen(
    isVisible: Boolean = true,
    onNavigateToBrowser: () -> Unit,
    onNavigateToSettings: () -> Unit = {},
    onNavigateToRecordings: () -> Unit = {},
    onNavigateToTone3000: (String?, String?, String?, Int, String?) -> Unit = { _, _, _, _, _ -> },
    onOpenModgui: (Int) -> Unit = {},
    onReplacePlugin: (Int) -> Unit = {},
    viewModel: RackViewModel = viewModel()
) {
    // Refresh rack state when screen becomes visible (e.g. returning from browser)
    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) {
                viewModel.refreshRack()
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    val context = LocalContext.current
    var currentBufferSize by remember { mutableIntStateOf(AudioSettingsManager.getBufferSize(context)) }

    // Refresh when navigating back to this screen (isVisible changes to true)
    LaunchedEffect(isVisible) {
        if (isVisible) {
            viewModel.refreshRack()
            currentBufferSize = AudioSettingsManager.getBufferSize(context)
        }
    }

    val isEngineRunning by viewModel.isEngineRunning.collectAsState()
    val latencyMs by viewModel.latencyMs.collectAsState()
    val inputLevel by viewModel.inputLevel.collectAsState()
    val outputLevel by viewModel.outputLevel.collectAsState()
    val cpuLoad by viewModel.cpuLoad.collectAsState()
    val xRunCount by viewModel.xRunCount.collectAsState()
    val inputClipping by viewModel.inputClipping.collectAsState()
    val outputClipping by viewModel.outputClipping.collectAsState()
    val rackPlugins by viewModel.rackPlugins.collectAsState()
    val errorMessage by viewModel.errorMessage.collectAsState()
    val presetMessage by viewModel.presetMessage.collectAsState()

    val wavLoaded by viewModel.wavLoaded.collectAsState()
    val wavDurationSec by viewModel.wavDurationSec.collectAsState()
    val wavPositionSec by viewModel.wavPositionSec.collectAsState()
    val isWavPlaying by viewModel.isWavPlaying.collectAsState()
    val loadedFileName by viewModel.loadedFileName.collectAsState()
    val wavRepeat by viewModel.wavRepeat.collectAsState()
    val wavProcessEffects by viewModel.wavProcessEffects.collectAsState()

    val isRecording by viewModel.isRecording.collectAsState()
    val recordingDurationSec by viewModel.recordingDurationSec.collectAsState()

    var showWavDialog by rememberSaveable { mutableStateOf(false) }

    val scope = rememberCoroutineScope()
    val wavFilePickerLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.GetContent()
    ) { uri: Uri? ->
        uri?.let { selectedUri ->
            scope.launch {
                val cacheFile = File(context.cacheDir, "wav_playback_${System.currentTimeMillis()}.wav")
                try {
                    withContext(Dispatchers.IO) {
                        context.contentResolver.openInputStream(selectedUri)?.use { input ->
                            cacheFile.outputStream().use { output ->
                                input.copyTo(output)
                            }
                        }
                    }
                    val fileName = selectedUri.lastPathSegment ?: cacheFile.name
                    viewModel.loadWav(cacheFile.absolutePath, fileName)
                } catch (e: Exception) {
                    viewModel.loadWav("")
                }
            }
        }
    }

    // Poll WAV position and handle repeat
    LaunchedEffect(wavLoaded) {
        while (wavLoaded) {
            viewModel.updateWavPosition()
            delay(200)
        }
    }
    LaunchedEffect(isWavPlaying, wavRepeat, wavPositionSec, wavDurationSec) {
        if (!isWavPlaying && wavRepeat && wavLoaded && wavDurationSec > 0 && wavPositionSec >= wavDurationSec - 0.3) {
            viewModel.wavRestart()
        }
    }

    var showPresetSheet by rememberSaveable { mutableStateOf(false) }
    val snackbarHostState = remember { SnackbarHostState() }

    LaunchedEffect(presetMessage) {
        presetMessage?.let {
            snackbarHostState.showSnackbar(it)
            viewModel.clearPresetMessage()
        }
    }

    Scaffold(
        contentWindowInsets = WindowInsets(0, 0, 0, 0),
        snackbarHost = { SnackbarHost(snackbarHostState) },
        topBar = {
            val statusText = if (isEngineRunning) {
                var s = "Running · %.1f ms · CPU %.0f%%".format(latencyMs, cpuLoad * 100f)
                if (xRunCount > 0) s += " · XRuns $xRunCount"
                if (inputClipping || outputClipping) s += " · Clip!"
                s
            } else "Stopped"
            val statusColor = if (inputClipping || outputClipping)
                MaterialTheme.colorScheme.error
            else
                MaterialTheme.colorScheme.onSurfaceVariant

            val cutoutPadding = WindowInsets.displayCutout.asPaddingValues()
            val cutoutTop = cutoutPadding.calculateTopPadding()

            // Get actual cutout bounds to center VU meters in each half
            val view = LocalView.current
            val density = LocalDensity.current
            val displayCutout = view.rootWindowInsets?.displayCutout
            val screenWidthPx = view.width.toFloat()
            // Find the cutout center zone (left edge to right edge) with generous padding
            val cutoutCenterStartDp: Dp
            val cutoutCenterEndDp: Dp
            if (displayCutout != null && displayCutout.boundingRects.isNotEmpty()) {
                val topCutout = displayCutout.boundingRects.firstOrNull { it.top == 0 }
                    ?: displayCutout.boundingRects[0]
                val extraPadding = with(density) { 4.dp.toPx() }
                cutoutCenterStartDp = with(density) { (topCutout.left - extraPadding).coerceAtLeast(0f).toDp() }
                cutoutCenterEndDp = with(density) { (screenWidthPx - topCutout.right - extraPadding).coerceAtLeast(0f).toDp() }
            } else {
                cutoutCenterStartDp = 0.dp
                cutoutCenterEndDp = 0.dp
            }

            Surface(
                tonalElevation = 3.dp,
                modifier = Modifier.fillMaxWidth()
            ) {
                Column(modifier = Modifier.fillMaxWidth()) {
                    // VU meters row — sits inside the cutout/status bar area
                    if (isEngineRunning) {
                        BoxWithConstraints(
                            modifier = Modifier
                                .fillMaxWidth()
                                .height(cutoutTop)
                        ) {
                            val halfWidth = minOf(cutoutCenterStartDp, cutoutCenterEndDp)
                            val meterWidth = halfWidth * 0.8f

                            Row(
                                modifier = Modifier.fillMaxSize(),
                                horizontalArrangement = Arrangement.SpaceEvenly,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                // In VU meter — centered in left half
                                Box(
                                    modifier = Modifier.width(halfWidth),
                                    contentAlignment = Alignment.Center
                                ) {
                                    VuMeter(
                                        modifier = Modifier.width(meterWidth),
                                        label = "In",
                                        level = inputLevel,
                                        clipping = inputClipping,
                                        onClippingTap = { viewModel.resetClipping() }
                                    )
                                }
                                // Out VU meter — centered in right half
                                Box(
                                    modifier = Modifier.width(halfWidth),
                                    contentAlignment = Alignment.Center
                                ) {
                                    VuMeter(
                                        modifier = Modifier.width(meterWidth),
                                        label = "Out",
                                        level = outputLevel,
                                        clipping = outputClipping,
                                        onClippingTap = { viewModel.resetClipping() }
                                    )
                                }
                            }
                        }
                    } else {
                        Spacer(modifier = Modifier.height(cutoutTop))
                    }

                    // Status row
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(horizontal = 8.dp, vertical = 2.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(
                            text = statusText,
                            style = MaterialTheme.typography.bodySmall,
                            color = statusColor,
                            modifier = Modifier
                                .weight(1f)
                                .clickable(enabled = inputClipping || outputClipping) {
                                    viewModel.resetClipping()
                                }
                        )
                        // Quick buffer size selector
                        val bufferLabel = AudioSettingsManager.BUFFER_SIZE_OPTIONS
                            .find { it.first == currentBufferSize }?.second ?: "Auto"
                        Box {
                            var showBufferMenu by remember { mutableStateOf(false) }
                            Row(
                                modifier = Modifier
                                    .clickable { showBufferMenu = true }
                                    .padding(horizontal = 8.dp, vertical = 4.dp),
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(2.dp)
                            ) {
                                Text(
                                    text = "Buffer:",
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                                Text(
                                    text = bufferLabel,
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.primary,
                                    fontWeight = FontWeight.Bold
                                )
                            }
                            DropdownMenu(
                                expanded = showBufferMenu,
                                onDismissRequest = { showBufferMenu = false }
                            ) {
                                AudioSettingsManager.BUFFER_SIZE_OPTIONS.forEach { (size, label) ->
                                    DropdownMenuItem(
                                        text = {
                                            Text(
                                                label,
                                                fontWeight = if (size == currentBufferSize) FontWeight.Bold else FontWeight.Normal
                                            )
                                        },
                                        onClick = {
                                            showBufferMenu = false
                                            if (size != currentBufferSize) {
                                                currentBufferSize = size
                                                AudioSettingsManager.setBufferSize(context, size)
                                                viewModel.restartEngine(context)
                                            }
                                        }
                                    )
                                }
                            }
                        }
                        Box {
                            var showOverflowMenu by remember { mutableStateOf(false) }
                            var showAboutDialog by remember { mutableStateOf(false) }
                            IconButton(
                                onClick = { showOverflowMenu = true },
                                modifier = Modifier.size(28.dp)
                            ) {
                                Icon(
                                    Icons.Default.MoreVert,
                                    contentDescription = "More options",
                                    modifier = Modifier.size(18.dp)
                                )
                            }
                            DropdownMenu(
                                expanded = showOverflowMenu,
                                onDismissRequest = { showOverflowMenu = false }
                            ) {
                                DropdownMenuItem(
                                    text = { Text("Settings") },
                                    onClick = {
                                        showOverflowMenu = false
                                        onNavigateToSettings()
                                    },
                                    leadingIcon = {
                                        Icon(
                                            Icons.Default.Settings,
                                            contentDescription = null,
                                            modifier = Modifier.size(20.dp)
                                        )
                                    }
                                )
                                DropdownMenuItem(
                                    text = { Text("Recordings") },
                                    onClick = {
                                        showOverflowMenu = false
                                        onNavigateToRecordings()
                                    },
                                    leadingIcon = {
                                        Icon(
                                            Icons.Default.Album,
                                            contentDescription = null,
                                            modifier = Modifier.size(20.dp)
                                        )
                                    }
                                )
                                DropdownMenuItem(
                                    text = { Text("TONE3000") },
                                    onClick = {
                                        showOverflowMenu = false
                                        onNavigateToTone3000(null, null, null, -1, null)
                                    },
                                    leadingIcon = {
                                        Icon(
                                            Icons.Default.Cloud,
                                            contentDescription = null,
                                            modifier = Modifier.size(20.dp)
                                        )
                                    }
                                )
                                DropdownMenuItem(
                                    text = { Text("About") },
                                    onClick = {
                                        showOverflowMenu = false
                                        showAboutDialog = true
                                    },
                                    leadingIcon = {
                                        Icon(
                                            Icons.Default.Info,
                                            contentDescription = null,
                                            modifier = Modifier.size(20.dp)
                                        )
                                    }
                                )
                            }
                            if (showAboutDialog) {
                                AboutDialog(
                                    onDismiss = { showAboutDialog = false }
                                )
                            }
                        }
                    }
                }
            }
        },
        bottomBar = {
            Column {
                if (wavLoaded) {
                    WavPlaybackBar(
                        fileName = loadedFileName,
                        positionSec = wavPositionSec,
                        durationSec = wavDurationSec,
                        isPlaying = isWavPlaying,
                        repeat = wavRepeat,
                        onPlayPause = {
                            if (isWavPlaying) viewModel.wavPause() else viewModel.wavPlay()
                        },
                        onRestart = { viewModel.wavRestart() },
                        onStop = {
                            viewModel.wavPause()
                            viewModel.wavSeek(0.0)
                        },
                        onToggleRepeat = { viewModel.wavToggleRepeat() },
                        onSeek = { viewModel.wavSeek(it) },
                        onClose = { viewModel.unloadWav() }
                    )
                }
                RackBottomBar(
                    isEngineRunning = isEngineRunning,
                    onToggleEngine = {
                        if (isEngineRunning) viewModel.stopEngine() else viewModel.restartEngine(context)
                    },
                    onAddPlugin = onNavigateToBrowser,
                    onOpenPresets = {
                        viewModel.refreshPresets(context)
                        showPresetSheet = true
                    },
                    isRecording = isRecording,
                    recordingDurationSec = recordingDurationSec,
                    onToggleRecording = { viewModel.toggleRecording(context) },
                    onOpenWav = { showWavDialog = true }
                )
            }
        }
    ) { padding ->

    // Preset bottom sheet
    if (showPresetSheet) {
        PresetBottomSheet(
            viewModel = viewModel,
            onDismiss = { showPresetSheet = false }
        )
    }

    // WAV load dialog
    var showRecordingPicker by remember { mutableStateOf(false) }
    if (showWavDialog) {
        WavLoadDialog(
            isEngineRunning = isEngineRunning,
            wavLoaded = wavLoaded,
            fileName = loadedFileName,
            positionSec = wavPositionSec,
            durationSec = wavDurationSec,
            isPlaying = isWavPlaying,
            repeat = wavRepeat,
            onLoadWav = { wavFilePickerLauncher.launch("audio/*") },
            onLoadRecordings = { showRecordingPicker = true },
            onPlayPause = { if (isWavPlaying) viewModel.wavPause() else viewModel.wavPlay() },
            onRestart = { viewModel.wavRestart() },
            onStop = { viewModel.wavPause(); viewModel.wavSeek(0.0) },
            onToggleRepeat = { viewModel.wavToggleRepeat() },
            onSeek = { viewModel.wavSeek(it) },
            onClose = { viewModel.unloadWav() },
            processEffects = wavProcessEffects,
            onToggleProcessEffects = { viewModel.wavToggleProcessEffects() },
            onDismiss = { showWavDialog = false }
        )
    }
    if (showRecordingPicker) {
        RecordingPickerDialog(
            onPickRecording = { path, isRaw ->
                viewModel.setWavProcessEffects(isRaw)
                viewModel.loadWav(path, java.io.File(path).name)
                showRecordingPicker = false
            },
            onDismiss = { showRecordingPicker = false }
        )
    }
        // Drag-reorder state — use scrollable Column so all plugin cards stay in composition (no re-render when scrolling)
        val localPlugins = remember { mutableStateOf(rackPlugins.toMutableStateList()) }
        val scrollState = rememberScrollState()
        val viewportTopInRoot = remember { mutableStateOf(0f) }
        val itemPositions = remember { mutableStateListOf<Pair<Int, Int>>() }
        val dragDropState = rememberScrollableDragDropState(
            itemPositions = { itemPositions.toList() },
            onMove = { from, to ->
                val list = localPlugins.value
                if (from in list.indices && to in list.indices) {
                    val item = list.removeAt(from)
                    list.add(to, item)
                }
            }
        )
        // Sync from viewmodel only when not dragging
        LaunchedEffect(rackPlugins, dragDropState.draggedIndex) {
            if (dragDropState.draggedIndex == null) {
                localPlugins.value = rackPlugins.toMutableStateList()
            }
        }
        // Keep positions list size in sync with plugin count (so onGloballyPositioned can set itemPositions[index])
        val pluginCount = localPlugins.value.size
        if (itemPositions.size < pluginCount) {
            repeat(pluginCount - itemPositions.size) { itemPositions.add(0 to 0) }
        } else if (itemPositions.size > pluginCount) {
            repeat(itemPositions.size - pluginCount) { itemPositions.removeAt(itemPositions.size - 1) }
        }

        val isDragging = dragDropState.draggedIndex != null
        val listScale by animateFloatAsState(
            targetValue = if (isDragging) 0.7f else 1f,
            animationSpec = tween(durationMillis = 250),
            label = "rackZoom"
        )

        BoxWithConstraints(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
        ) {
            val viewportHeight = maxHeight
            val contentHeight = (viewportHeight / listScale).coerceAtLeast(viewportHeight)
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .verticalScroll(scrollState)
                    .onGloballyPositioned { coords ->
                        viewportTopInRoot.value = coords.boundsInWindow().top
                    }
                    .pointerInput(Unit) {
                    detectDragGesturesAfterLongPress(
                        onDragStart = { offset ->
                            dragDropState.onDragStart(offset.y)
                        },
                        onDrag = { change, dragAmount ->
                            change.consume()
                            dragDropState.onDrag(dragAmount.y)
                        },
                        onDragEnd = {
                            dragDropState.onDragEnd()?.let { (from, to) ->
                                viewModel.reorderPlugins(from, to)
                            }
                        },
                        onDragCancel = {
                            dragDropState.onDragCancel()
                        }
                    )
                }
            ) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .then(if (isDragging) Modifier.height(contentHeight) else Modifier.wrapContentHeight())
                        .graphicsLayer {
                            scaleX = listScale
                            scaleY = listScale
                            transformOrigin = androidx.compose.ui.graphics.TransformOrigin(0.5f, 0f)
                        }
                ) {
                    Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .wrapContentHeight()
                    ) {
            Spacer(modifier = Modifier.height(16.dp))
            errorMessage?.let { error ->
                Card(
                    modifier = Modifier.fillMaxWidth(),
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.errorContainer
                    )
                ) {
                    Text(
                        text = error,
                        modifier = Modifier.padding(16.dp),
                        color = MaterialTheme.colorScheme.onErrorContainer
                    )
                }
                Spacer(modifier = Modifier.height(8.dp))
            }
            if (localPlugins.value.isEmpty()) {
                EmptyRackPlaceholder(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 32.dp)
                )
            } else {
                Column(
                    modifier = Modifier.fillMaxWidth(),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    localPlugins.value.forEachIndexed { index, plugin ->
                        key(plugin.instanceId) {
                        val isDragged = dragDropState.draggedIndex == index
                        val nativeIndex = plugin.index
                        Box(
                            modifier = Modifier
                                .fillMaxWidth()
                                .onGloballyPositioned { coords ->
                                    if (index < itemPositions.size) {
                                        val b = coords.boundsInWindow()
                                        itemPositions[index] = ((b.top - viewportTopInRoot.value).toInt() to b.height.toInt())
                                    }
                                }
                        ) {
                            PluginCard(
                                plugin = plugin,
                                pluginIndex = nativeIndex,
                                viewModel = viewModel,
                                onRemove = { viewModel.removePlugin(nativeIndex) },
                                onReplace = { onReplacePlugin(nativeIndex) },
                                onOpenModgui = onOpenModgui,
                                onNavigateToTone3000 = onNavigateToTone3000,
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .zIndex(if (isDragged) 1f else 0f)
                                    .graphicsLayer {
                                        translationY = if (isDragged) dragDropState.draggedOffset else 0f
                                        scaleX = if (isDragged) 1.05f else 1f
                                        scaleY = if (isDragged) 1.05f else 1f
                                        shadowElevation = if (isDragged) 16f else 0f
                                    }
                            )
                        }
                        }
                    }
                }
            }
            Spacer(modifier = Modifier.height(16.dp))
                    }
                }
            }
        }
    }
}


@Composable
private fun EmptyRackPlaceholder(modifier: Modifier = Modifier) {
    Box(
        modifier = modifier,
        contentAlignment = Alignment.Center
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Text(
                text = "No plugins in rack",
                style = MaterialTheme.typography.titleMedium
            )
            Text(
                text = "Tap + to add plugins",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

private fun defaultScaleForAspectRatio(width: Int, height: Int): Float =
    if (width >= height) 1.0f else 0.4f

/** Fixed height for the X11 plugin viewport so the X server root window matches the plugin box (not full screen). */
private val x11ViewportHeight = 360.dp

@Composable
private fun VuMeter(
    modifier: Modifier = Modifier,
    label: String,
    level: Float,
    clipping: Boolean,
    onClippingTap: () -> Unit
) {
    val barHeight = 6.dp
    val trackColor = MaterialTheme.colorScheme.surfaceVariant
    val green = Color(0xFF4CAF50)
    val yellow = Color(0xFFFFC107)
    val red = Color(0xFFF44336)
    Column(
        modifier = modifier,
        verticalArrangement = Arrangement.spacedBy(2.dp)
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp)
        ) {
            Text(
                text = label,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Box(
                modifier = Modifier
                    .weight(1f)
                    .height(barHeight)
                    .clip(RoundedCornerShape(4.dp))
                    .background(trackColor)
            ) {
                Box(
                    modifier = Modifier
                        .fillMaxHeight()
                        .fillMaxWidth(level.coerceIn(0f, 1f))
                        .background(
                            brush = Brush.horizontalGradient(
                                colors = listOf(green, yellow, red),
                                startX = 0f,
                                endX = 500f
                            ),
                            shape = RoundedCornerShape(4.dp)
                        )
                )
            }
            Box(
                modifier = Modifier
                    .size(8.dp)
                    .clip(CircleShape)
                    .background(
                        if (clipping) red
                        else MaterialTheme.colorScheme.surfaceVariant
                    )
                    .clickable(onClick = onClippingTap),
                contentAlignment = Alignment.Center
            ) { }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PluginCard(
    plugin: RackPlugin,
    pluginIndex: Int,
    viewModel: RackViewModel,
    onRemove: () -> Unit,
    onReplace: () -> Unit = {},
    onOpenModgui: (Int) -> Unit = {},
    onNavigateToTone3000: (String?, String?, String?, Int, String?) -> Unit = { _, _, _, _, _ -> },
    modifier: Modifier = Modifier.fillMaxWidth()
) {
    var expanded by rememberSaveable { mutableStateOf(true) }
    val pluginInfo = remember(pluginIndex) { RackManager.getRackPluginInfo(pluginIndex) }

    Card(
        modifier = modifier,
        colors = CardDefaults.cardColors(containerColor = Color.Transparent),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp)
    ) {
        Column(
            modifier = Modifier.fillMaxWidth()
        ) {
            // Parameter controls / plugin UI — always in composition to avoid re-renders
            if (pluginInfo != null) {
                // Determine which UI modes are available
                val availableUiTypes = remember(pluginInfo) {
                    buildList {
                        if (pluginInfo.hasX11Ui) add(UiType.X11)
                        if (pluginInfo.hasModgui) add(UiType.MODGUI)
                        add(UiType.SLIDERS)
                    }
                }
                var currentUiMode by rememberSaveable(pluginInfo.fullId) {
                    mutableStateOf(viewModel.getPreferredUiTypeForPlugin(pluginInfo))
                }
                LaunchedEffect(pluginIndex, pluginInfo, currentUiMode) {
                    Log.i("GuitarRackCraft.UI", "Plugin[$pluginIndex] ${pluginInfo.name}: chosen UI mode=$currentUiMode (preferred=${pluginInfo.preferredUiType}, available=${pluginInfo.guiTypes})")
                }

                var x11UserScale by rememberSaveable { mutableStateOf(Float.NaN) }
                var modguiUserScale by rememberSaveable { mutableStateOf(Float.NaN) }

                // Top bar: context menu + plugin name + expand/remove
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(
                            MaterialTheme.colorScheme.surfaceColorAtElevation(2.dp),
                            RoundedCornerShape(8.dp)
                        )
                        .padding(horizontal = 2.dp, vertical = 0.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    // Context menu button
                    var showContextMenu by remember { mutableStateOf(false) }
                    Box {
                        IconButton(
                            onClick = { showContextMenu = true },
                            modifier = Modifier.size(32.dp)
                        ) {
                            Icon(Icons.Default.MoreVert, contentDescription = "Options", modifier = Modifier.size(20.dp))
                        }
                        DropdownMenu(
                            expanded = showContextMenu,
                            onDismissRequest = { showContextMenu = false }
                        ) {
                            // UI mode selection
                            if (availableUiTypes.size > 1) {
                                availableUiTypes.forEach { uiType ->
                                    val isSelected = currentUiMode == uiType
                                    val icon = when (uiType) {
                                        UiType.X11 -> Icons.Default.DesktopWindows
                                        UiType.MODGUI -> Icons.Default.Dashboard
                                        UiType.SLIDERS -> Icons.Default.Tune
                                    }
                                    DropdownMenuItem(
                                        text = {
                                            Text(
                                                uiType.displayName,
                                                fontWeight = if (isSelected) FontWeight.Bold else FontWeight.Normal
                                            )
                                        },
                                        onClick = {
                                            currentUiMode = uiType
                                            viewModel.setPreferredUiTypeForPlugin(pluginInfo.fullId, uiType)
                                            showContextMenu = false
                                        },
                                        leadingIcon = {
                                            Icon(icon, contentDescription = null, modifier = Modifier.size(20.dp))
                                        },
                                        modifier = if (uiType == UiType.X11) Modifier.testTag("plugin_native_ui_button") else Modifier
                                    )
                                }
                                Divider()
                            }
                            // Scale slider
                            val hasScale = (currentUiMode == UiType.X11 && !x11UserScale.isNaN()) ||
                                    (currentUiMode == UiType.MODGUI && !modguiUserScale.isNaN())
                            if (hasScale) {
                                val scaleVal = when (currentUiMode) {
                                    UiType.X11 -> if (x11UserScale.isNaN()) 1f else x11UserScale
                                    UiType.MODGUI -> if (modguiUserScale.isNaN()) 1f else modguiUserScale
                                    else -> 1f
                                }
                                Row(
                                    modifier = Modifier.padding(horizontal = 12.dp),
                                    verticalAlignment = Alignment.CenterVertically
                                ) {
                                    Icon(Icons.Default.Fullscreen, contentDescription = null, modifier = Modifier.size(20.dp))
                                    Slider(
                                        value = scaleVal,
                                        onValueChange = { v ->
                                            when (currentUiMode) {
                                                UiType.X11 -> x11UserScale = v
                                                UiType.MODGUI -> modguiUserScale = v
                                                else -> {}
                                            }
                                        },
                                        valueRange = 0.3f..1f,
                                        modifier = Modifier.width(150.dp).padding(horizontal = 4.dp)
                                    )
                                }
                                Divider()
                            }
                        }
                    }

                    Text(
                        text = plugin.name,
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier.weight(1f)
                    )
                    // Collapse
                    IconButton(
                        onClick = { expanded = !expanded },
                        modifier = Modifier.size(32.dp).testTag("plugin_card_expand")
                    ) {
                        Icon(
                            if (expanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
                            contentDescription = if (expanded) "Collapse" else "Expand",
                            modifier = Modifier.size(20.dp)
                        )
                    }
                    // Fullscreen
                    if (pluginInfo.hasModgui || pluginInfo.hasX11Ui) {
                        IconButton(
                            onClick = { onOpenModgui(pluginIndex) },
                            modifier = Modifier.size(32.dp)
                        ) {
                            Icon(
                                Icons.Default.Fullscreen,
                                contentDescription = "Fullscreen",
                                modifier = Modifier.size(20.dp)
                            )
                        }
                    }
                    // Replace
                    IconButton(
                        onClick = onReplace,
                        modifier = Modifier.size(32.dp)
                    ) {
                        Icon(
                            Icons.Default.SwapHoriz,
                            contentDescription = "Replace",
                            modifier = Modifier.size(20.dp)
                        )
                    }
                    // Remove
                    IconButton(
                        onClick = onRemove,
                        modifier = Modifier.size(32.dp)
                    ) {
                        Icon(
                            Icons.Default.Close,
                            contentDescription = "Remove",
                            modifier = Modifier.size(20.dp)
                        )
                    }
                }

                val contentModifier = if (expanded) Modifier else Modifier.height(0.dp).alpha(0f)
                Column(modifier = contentModifier) {

                Spacer(modifier = Modifier.height(8.dp))

                // Allocate X11 display number for this plugin — always kept alive while in rack.
                // Display is only released when plugin is removed from rack (onDispose).
                var x11DisplayNumber by remember {
                    mutableStateOf(
                        if (pluginInfo.hasX11Ui) X11DisplayManager.allocateDisplay() else -1
                    )
                }

                // Cleanup X11 display when plugin is removed from rack
                DisposableEffect(Unit) {
                    onDispose {
                        if (x11DisplayNumber >= 0) {
                            Log.i("GuitarRackCraft.UI", "Plugin[$pluginIndex]: Releasing X11 display $x11DisplayNumber (removed from rack)")
                            val dispNum = x11DisplayNumber
                            val pIdx = pluginIndex
                            x11DisplayNumber = -1
                            X11DisplayManager.teardownExecutor.execute {
                                X11Bridge.destroyPluginUI(pIdx)
                                X11Bridge.detachAndDestroyX11DisplayIfExists(dispNum)
                                X11DisplayManager.releaseDisplay(dispNum)
                            }
                        }
                    }
                }

                // --- X11 UI file picker (ui:requestValue) ---
                val context = LocalContext.current
                val scope = rememberCoroutineScope()
                var x11FileRequestPending by remember { mutableStateOf<Pair<Int, String>?>(null) }
                var showX11FilePicker by remember { mutableStateOf(false) }

                val x11FilePicker = rememberLauncherForActivityResult(
                    contract = ActivityResultContracts.OpenDocument()
                ) { uri ->
                    val req = x11FileRequestPending ?: return@rememberLauncherForActivityResult
                    if (uri != null) {
                        scope.launch(Dispatchers.IO) {
                            val fileName = uri.lastPathSegment?.substringAfterLast('/') ?: "model"
                            val pickerConfig = getX11FilePickerConfig(req.second)
                            val storageDir = pickerConfig.storageDirs.first()
                            val destDir = java.io.File(context.filesDir, storageDir)
                            destDir.mkdirs()
                            val destFile = java.io.File(destDir, fileName)
                            context.contentResolver.openInputStream(uri)?.use { input ->
                                destFile.outputStream().use { output -> input.copyTo(output) }
                            }
                            withContext(Dispatchers.Main) {
                                viewModel.setPluginFilePath(req.first, req.second, destFile.absolutePath)
                                X11Bridge.deliverFileToPluginUI(req.first, req.second, destFile.absolutePath)
                            }
                        }
                    }
                    x11FileRequestPending = null
                }

                // Poll for X11 UI file requests when X11 UI is visible
                val x11UiActive = currentUiMode == UiType.X11
                LaunchedEffect(x11UiActive, x11DisplayNumber) {
                    if (x11UiActive && x11DisplayNumber >= 0) {
                        while (isActive) {
                            val req = X11Bridge.pollFileRequest()
                            if (req != null && req.size == 2) {
                                x11FileRequestPending = Pair(req[0].toInt(), req[1])
                                showX11FilePicker = true
                            }
                            delay(100)
                        }
                    }
                }

                // X11 file picker dialog
                if (showX11FilePicker && x11FileRequestPending != null) {
                    val req = x11FileRequestPending!!
                    val pickerConfig = remember(req.second) { getX11FilePickerConfig(req.second) }
                    X11FilePickerDialog(
                        config = pickerConfig,
                        sourcePluginIndex = req.first,
                        sourcePropertyUri = req.second,
                        onFileSelected = { path ->
                            showX11FilePicker = false
                            viewModel.setPluginFilePath(req.first, req.second, path)
                            X11Bridge.deliverFileToPluginUI(req.first, req.second, path)
                            x11FileRequestPending = null
                        },
                        onBrowseFiles = {
                            showX11FilePicker = false
                            x11FilePicker.launch(arrayOf("*/*"))
                        },
                        onNavigateToTone3000 = onNavigateToTone3000,
                        onDismiss = {
                            showX11FilePicker = false
                            x11FileRequestPending = null
                        }
                    )
                }

                // CRITICAL: Always keep PluginX11UiView in composition to avoid TextureView destruction.
                // When switching away from X11 mode, we hide it instead of removing it.
                // This prevents the Android graphics driver from destroying shared mutexes.
                var x11PluginNaturalW by remember { mutableStateOf(0) }
                var x11PluginNaturalH by remember { mutableStateOf(0) }
                var x11UIScale by remember { mutableStateOf(1f) }
                var x11UiReady by rememberSaveable { mutableStateOf(false) }
                var x11OnScreen by remember { mutableStateOf(true) }
                Column(modifier = Modifier.fillMaxWidth()) {
                    BoxWithConstraints(modifier = Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                        val userScale = if (x11UserScale.isNaN()) 1f else x11UserScale
                        val effectiveScale = (x11UIScale * userScale).coerceIn(0.3f, 1f)
                        val x11Height = if (x11PluginNaturalW > 0 && x11PluginNaturalH > 0) {
                            maxWidth * effectiveScale * (x11PluginNaturalH.toFloat() / x11PluginNaturalW.toFloat())
                        } else {
                            x11ViewportHeight
                        }
                        val x11Visible = currentUiMode == UiType.X11
                        val x11ShowContent = x11Visible && x11UiReady
                        Box(
                            modifier = Modifier
                                .fillMaxWidth(fraction = effectiveScale)
                                .height(if (x11Visible) x11Height else 0.dp)
                                .alpha(if (x11ShowContent) 1f else 0f)
                                .testTag("x11_plugin_viewport")
                                .clip(RoundedCornerShape(4.dp))
                                .onGloballyPositioned { coords ->
                                    val bounds = coords.boundsInWindow()
                                    x11OnScreen = !bounds.isEmpty
                                }
                        ) {
                            if (x11DisplayNumber >= 0) {
                                // Keep X11 rendering when card is off-screen (don't use x11OnScreen for isVisible).
                                PluginX11UiView(
                                    pluginIndex = pluginIndex,
                                    displayNumber = x11DisplayNumber,
                                    isVisible = expanded && currentUiMode == UiType.X11,
                                    shouldDestroyOnDispose = true,
                                    modifier = Modifier.fillMaxSize(),
                                    onPluginSizeKnown = { w, h, scale ->
                                        x11PluginNaturalW = w
                                        x11PluginNaturalH = h
                                        x11UIScale = scale
                                        if (x11UserScale.isNaN()) {
                                            x11UserScale = defaultScaleForAspectRatio(w, h)
                                        }
                                        x11UiReady = true
                                    }
                                )
                            }
                        }
                    }
                }

                // Model picker shared state — declared before modgui block so it can be referenced there
                val modelConfig = remember(pluginInfo.id) { getModelPluginConfig(pluginInfo.id) }
                var modelFilePickerTrigger by remember { mutableStateOf(0) }
                var modelActiveModelName by rememberSaveable { mutableStateOf<String?>(null) }
                var modelFiles by remember { mutableStateOf<List<java.io.File>>(emptyList()) }

                // Observe model loaded events from external sources (e.g. Tone3000 download)
                LaunchedEffect(pluginIndex) {
                    RackManager.modelLoadedEvents.collect { event ->
                        if (event.pluginIndex == pluginIndex) {
                            modelActiveModelName = event.modelName
                        }
                    }
                }

                // Keep modgui always in composition (like X11) so it doesn't re-render on each switch.
                if (pluginInfo.hasModgui) {
                    var modguiContentWidth by remember { mutableStateOf(0) }
                    var modguiContentHeight by remember { mutableStateOf(0) }
                    var modguiReady by rememberSaveable { mutableStateOf(false) }
                    var modguiOnScreen by remember { mutableStateOf(true) }
                    @Suppress("NAME_SHADOWING") val density = LocalDensity.current
                    val modguiModeActive = currentUiMode == UiType.MODGUI
                    val modguiVisible = modguiModeActive && modguiReady
                    Column(modifier = Modifier.fillMaxWidth()) {
                        BoxWithConstraints(
                            modifier = Modifier
                                .fillMaxWidth()
                                .then(if (!modguiModeActive) Modifier.height(0.dp) else Modifier)
                                .alpha(if (modguiVisible) 1f else 0f)
                                .onGloballyPositioned { coords ->
                                    val bounds = coords.boundsInWindow()
                                    modguiOnScreen = !bounds.isEmpty
                                },
                            contentAlignment = Alignment.Center
                        ) {
                            val effectiveScale = (if (modguiUserScale.isNaN()) 1f else modguiUserScale).coerceIn(0.3f, 1f)
                            val heightDp = if (modguiContentWidth > 0 && modguiContentHeight > 0) {
                                val availableWidthPx = with(density) { maxWidth.toPx() } * effectiveScale
                                val scale = availableWidthPx / modguiContentWidth.toFloat()
                                with(density) { (modguiContentHeight * scale).toDp() }
                            } else {
                                200.dp
                            }
                            InlineModguiView(
                                pluginIndex = pluginIndex,
                                pluginInfo = pluginInfo,
                                isVisible = expanded && modguiOnScreen,
                                modifier = Modifier
                                    .fillMaxWidth(fraction = effectiveScale)
                                    .height(heightDp),
                                onContentSize = { w, h ->
                                    modguiContentWidth = w
                                    modguiContentHeight = h
                                    if (modguiUserScale.isNaN()) {
                                        modguiUserScale = defaultScaleForAspectRatio(w, h)
                                    }
                                    modguiReady = true
                                },
                                onFilePickerRequested = { modelFilePickerTrigger++ },
                                modelDisplayName = modelActiveModelName,
                                modelFiles = modelFiles,
                                onModelSelected = { path ->
                                    modelActiveModelName = java.io.File(path).nameWithoutExtension
                                    modelConfig?.let { cfg ->
                                        viewModel.setPluginFilePath(
                                            pluginIndex,
                                            cfg.propertyUri,
                                            path
                                        )
                                    }
                                }
                            )
                        }
                    }
                }

                // ModelPicker: headless, kept in composition for the file picker launcher
                if (modelConfig != null) {
                    val initialPlatform = when {
                        pluginInfo.id.contains("neural-amp-modeler") || pluginInfo.id.contains("neuralrack", ignoreCase = true) -> "nam"
                        pluginInfo.id.contains("aidadsp") -> "aida-x"
                        else -> null
                    }
                    val initialTag = if (initialPlatform == "aida-x") "aida-x" else null
                    ModelPicker(
                        pluginIndex = pluginIndex,
                        viewModel = viewModel,
                        config = modelConfig,
                        externalPickerTrigger = modelFilePickerTrigger,
                        onActiveModelChanged = { modelActiveModelName = it },
                        onModelFilesChanged = { modelFiles = it },
                        onNavigateToTone3000 = {
                            val sourceSlot = modelConfig.propertyUri.substringAfterLast('#', "").ifEmpty { null }
                            onNavigateToTone3000(initialTag, null, initialPlatform, pluginIndex, sourceSlot)
                        }
                    )
                }

                when (currentUiMode) {
                    UiType.X11 -> {
                        // X11 view is always present above, just show it
                        LaunchedEffect(pluginIndex, pluginInfo) {
                            Log.i("GuitarRackCraft.UI", "Plugin[$pluginIndex] ${pluginInfo.name}: showing X11 UI (portrait)")
                        }
                    }
                    UiType.MODGUI -> {
                        // Modgui is always present above, nothing extra needed
                    }
                    UiType.SLIDERS -> {
                      Column(modifier = Modifier.padding(horizontal = 12.dp)) {
                        if (modelConfig != null) {
                            var modelDropdownExpanded by remember { mutableStateOf(false) }
                            ExposedDropdownMenuBox(
                                expanded = modelDropdownExpanded,
                                onExpandedChange = {
                                    if (modelFiles.size > 1) {
                                        modelDropdownExpanded = it
                                    } else {
                                        modelFilePickerTrigger++
                                    }
                                }
                            ) {
                                OutlinedTextField(
                                    value = modelActiveModelName ?: modelConfig.placeholder,
                                    onValueChange = {},
                                    readOnly = true,
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .menuAnchor(),
                                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = modelDropdownExpanded) }
                                )
                                ExposedDropdownMenu(
                                    expanded = modelDropdownExpanded,
                                    onDismissRequest = { modelDropdownExpanded = false }
                                ) {
                                    DropdownMenuItem(
                                        text = { Text("\u2026", fontWeight = androidx.compose.ui.text.font.FontWeight.Bold) },
                                        onClick = {
                                            modelDropdownExpanded = false
                                            modelFilePickerTrigger++
                                        }
                                    )
                                    modelFiles.forEach { modelFile ->
                                        DropdownMenuItem(
                                            text = { Text(modelFile.nameWithoutExtension) },
                                            onClick = {
                                                modelDropdownExpanded = false
                                                modelActiveModelName = modelFile.nameWithoutExtension
                                                viewModel.setPluginFilePath(
                                                    pluginIndex,
                                                    modelConfig.propertyUri,
                                                    modelFile.absolutePath
                                                )
                                            }
                                        )
                                    }
                                }
                            }
                            Spacer(modifier = Modifier.height(8.dp))
                        }
                        val controlPorts = pluginInfo.controlPorts
                        if (controlPorts.isNotEmpty()) {
                            Divider()
                            Spacer(modifier = Modifier.height(8.dp))
                            Column(
                                verticalArrangement = Arrangement.spacedBy(12.dp)
                            ) {
                                controlPorts.forEach { port ->
                                    ParameterControl(
                                        port = port,
                                        pluginIndex = pluginIndex,
                                        viewModel = viewModel
                                    )
                                }
                            }
                        } else if (modelConfig == null) {
                            Text(
                                text = "No control parameters available",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                      }
                    }
                }
                } // end contentModifier Column
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ParameterControl(
    port: com.varcain.guitarrackcraft.engine.PortInfo,
    pluginIndex: Int,
    viewModel: RackViewModel
) {
    val currentValue = remember { mutableStateOf(viewModel.getParameter(pluginIndex, port.index)) }
    var isUserInteracting by remember { mutableStateOf(false) }

    // Poll native parameter value periodically so that changes made via the X11 UI
    // (or other external sources) are reflected in the Android slider controls.
    LaunchedEffect(pluginIndex, port.index) {
        while (true) {
            delay(200)
            if (!isUserInteracting) {
                val nativeValue = viewModel.getParameter(pluginIndex, port.index)
                if (kotlin.math.abs(nativeValue - currentValue.value) > 1e-5f) {
                    currentValue.value = nativeValue
                }
            }
        }
    }

    Column {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = port.name.ifEmpty { port.symbol },
                style = MaterialTheme.typography.bodyMedium
            )
            Text(
                text = if (port.scalePoints.isNotEmpty()) {
                    port.scalePoints.find { kotlin.math.abs(it.value - currentValue.value) < 1e-6f }
                        ?.label ?: "%.2f".format(currentValue.value)
                } else {
                    "%.2f".format(currentValue.value)
                },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }

        if (port.isToggle) {
            // Toggle port: switch
            Switch(
                checked = currentValue.value > 0.5f,
                onCheckedChange = { checked ->
                    val newValue = if (checked) port.maxValue else port.minValue
                    currentValue.value = newValue
                    viewModel.setParameter(pluginIndex, port.index, newValue)
                }
            )
        } else if (port.scalePoints.isNotEmpty()) {
            // Enumeration port: dropdown
            var expanded by remember { mutableStateOf(false) }
            ExposedDropdownMenuBox(
                expanded = expanded,
                onExpandedChange = { expanded = it }
            ) {
                OutlinedTextField(
                    value = port.scalePoints.find { kotlin.math.abs(it.value - currentValue.value) < 1e-6f }
                        ?.label ?: "%.2f".format(currentValue.value),
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
                    port.scalePoints.forEach { sp ->
                        DropdownMenuItem(
                            text = { Text(sp.label) },
                            onClick = {
                                currentValue.value = sp.value
                                viewModel.setParameter(pluginIndex, port.index, sp.value)
                                expanded = false
                            }
                        )
                    }
                }
            }
        } else {
            // Continuous port: slider
            Slider(
                value = currentValue.value,
                onValueChange = { newValue ->
                    isUserInteracting = true
                    currentValue.value = newValue
                    viewModel.setParameter(pluginIndex, port.index, newValue)
                },
                onValueChangeFinished = {
                    isUserInteracting = false
                },
                valueRange = port.minValue..port.maxValue,
                modifier = Modifier.fillMaxWidth()
            )
        }
    }
}

private data class ModelPluginConfig(
    val propertyUri: String,
    val extensions: Set<String>,
    val storageDirs: List<String>,
    val placeholder: String,
    val builtInItems: List<Pair<String, String>> = emptyList()
)

private fun getModelPluginConfig(pluginId: String): ModelPluginConfig? = when {
    pluginId.contains("neural-amp-modeler") || pluginId.contains("neuralrack", ignoreCase = true) -> ModelPluginConfig(
        propertyUri = if (pluginId.contains("neural-amp-modeler")) "http://github.com/mikeoliphant/neural-amp-modeler-lv2#model" else "urn:brummer:neuralrack#Neural_Model",
        extensions = setOf("nam", "nammodel", "json"),
        storageDirs = listOf("neural_models", "aidax_models"),
        placeholder = "No model loaded",
        builtInItems = if (pluginId.contains("neural-amp-modeler")) listOf("Default: NAM Profile" to "default") else emptyList()
    )
    pluginId.contains("aidadsp") -> ModelPluginConfig(
        propertyUri = "http://aidadsp.cc/plugins/aidadsp-bundle/rt-neural-generic#json",
        extensions = setOf("json", "aidax", "aidadspmodel"),
        storageDirs = listOf("aidax_models"),
        placeholder = "No model loaded",
        builtInItems = listOf("Default: California Clean" to "default")
    )
    pluginId.contains("ImpulseLoader") -> ModelPluginConfig(
        propertyUri = "urn:brummer:ImpulseLoader#irfile",
        extensions = setOf("wav"),
        storageDirs = listOf("ir_models"),
        placeholder = "No IR loaded"
    )
    else -> null
}

private data class X11FilePickerConfig(
    val title: String,
    val storageDirs: List<String>,
    val extensions: Set<String>,
    val builtInItems: List<Pair<String, String>> // displayName to deliveryValue
)

private fun getX11FilePickerConfig(propertyUri: String): X11FilePickerConfig = when {
    propertyUri.endsWith("#json") || propertyUri.contains("rt-neural-generic#json") -> X11FilePickerConfig(
        title = "Select AIDA-X Model",
        storageDirs = listOf("aidax_models"),
        extensions = setOf("json", "aidax", "aidadspmodel"),
        builtInItems = listOf("Default: California Clean" to "default")
    )
    propertyUri.endsWith("#cabinet") || propertyUri.contains("irfile") -> X11FilePickerConfig(
        title = "Select Impulse Response",
        storageDirs = listOf("ir_models"),
        extensions = setOf("wav"),
        builtInItems = if (propertyUri.endsWith("#cabinet")) listOf("Default: V30 Audix i5" to "default") else emptyList()
    )
    propertyUri.contains("neural-amp-modeler") || propertyUri.contains("neuralrack#Neural_Model") -> X11FilePickerConfig(
        title = "Select Neural Model",
        storageDirs = listOf("neural_models", "aidax_models"),
        extensions = setOf("nam", "nammodel", "json"),
        builtInItems = if (propertyUri.contains("neural-amp-modeler")) listOf("Default: NAM Profile" to "default") else emptyList()
    )
    else -> X11FilePickerConfig(
        title = "Select File",
        storageDirs = listOf("aidax_models"),
        extensions = emptySet(),
        builtInItems = emptyList()
    )
}

@Composable
private fun GenericFilePickerDialog(
    title: String,
    storageDirs: List<String>,
    extensions: Set<String>,
    builtInItems: List<Pair<String, String>>,
    onFileSelected: (String) -> Unit,
    onBrowseFiles: () -> Unit,
    onNavigateToTone3000: () -> Unit,
    onDismiss: () -> Unit
) {
    val context = LocalContext.current
    var existingFiles by remember { mutableStateOf<List<java.io.File>>(emptyList()) }
    var favorites by remember { mutableStateOf(com.varcain.guitarrackcraft.engine.ModelFavoritesManager.getFavorites(context)) }
    var expandedGroups by remember { mutableStateOf(setOf<String>()) }

    fun refreshFiles() {
        val allFiles = mutableListOf<java.io.File>()
        storageDirs.forEach { dirName ->
            val dir = java.io.File(context.filesDir, dirName)
            if (dir.exists()) {
                allFiles.addAll(
                    dir.walk()
                        .filter { it.isFile }
                        .filter { extensions.isEmpty() || it.extension.lowercase() in extensions }
                        .toList()
                )
            }
        }
        existingFiles = allFiles.sortedBy { it.name.lowercase() }
    }

    LaunchedEffect(storageDirs) {
        withContext(Dispatchers.IO) {
            refreshFiles()
        }
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            LazyColumn(modifier = Modifier.fillMaxWidth().heightIn(max = 400.dp)) {
                item {
                    Column {
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable { onBrowseFiles() }
                                .padding(vertical = 12.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(12.dp)
                        ) {
                            Icon(Icons.Default.Folder, contentDescription = null)
                            Text("Browse files\u2026", fontWeight = FontWeight.Bold)
                        }
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable { 
                                    onDismiss()
                                    onNavigateToTone3000() 
                                }
                                .padding(vertical = 12.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(12.dp)
                        ) {
                            Icon(Icons.Default.Cloud, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
                            Text("Browse TONE3000", fontWeight = FontWeight.Bold, color = MaterialTheme.colorScheme.primary)
                        }
                    }
                }

                val favoriteFiles = existingFiles.filter { favorites.contains(it.absolutePath) }
                val nonFavoriteFiles = existingFiles.filter { !favorites.contains(it.absolutePath) }

                if (favoriteFiles.isNotEmpty()) {
                    item { Divider() }
                    item {
                        Text(
                            "Favorites",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.primary,
                            modifier = Modifier.padding(top = 8.dp, bottom = 4.dp)
                        )
                    }
                    items(favoriteFiles) { file ->
                        FileItemRow(
                            name = file.nameWithoutExtension,
                            path = file.absolutePath,
                            isFavorite = true,
                            onSelect = { onFileSelected(it) },
                            onToggleFavorite = {
                                com.varcain.guitarrackcraft.engine.ModelFavoritesManager.toggleFavorite(context, it)
                                favorites = com.varcain.guitarrackcraft.engine.ModelFavoritesManager.getFavorites(context)
                            },
                            onDelete = {
                                java.io.File(it).delete()
                                refreshFiles()
                            }
                        )
                    }
                }

                if (builtInItems.isNotEmpty() || nonFavoriteFiles.isNotEmpty()) {
                    item { Divider() }
                }

                if (builtInItems.isNotEmpty()) {
                    item {
                        Text(
                            "Built-in",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(top = 8.dp, bottom = 4.dp)
                        )
                    }
                    items(builtInItems) { (name, value) ->
                        FileItemRow(
                            name = name,
                            path = value,
                            isFavorite = favorites.contains(value),
                            canDelete = false,
                            onSelect = { onFileSelected(it) },
                            onToggleFavorite = {
                                com.varcain.guitarrackcraft.engine.ModelFavoritesManager.toggleFavorite(context, it)
                                favorites = com.varcain.guitarrackcraft.engine.ModelFavoritesManager.getFavorites(context)
                            }
                        )
                    }
                }

                if (nonFavoriteFiles.isNotEmpty()) {
                    item {
                        Text(
                            "On device",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(top = 8.dp, bottom = 4.dp)
                        )
                    }

                    val groups = nonFavoriteFiles.groupBy { file ->
                        val parent = file.parentFile ?: return@groupBy null
                        val isAtRoot = storageDirs.any { dirName -> 
                            parent.absolutePath == java.io.File(context.filesDir, dirName).absolutePath 
                        }
                        if (isAtRoot) null else parent.name
                    }

                    groups.forEach { (groupName, files) ->
                        if (groupName == null) {
                            items(files) { file ->
                                FileItemRow(
                                    name = file.nameWithoutExtension,
                                    path = file.absolutePath,
                                    isFavorite = false,
                                    onSelect = { onFileSelected(it) },
                                    onToggleFavorite = {
                                        com.varcain.guitarrackcraft.engine.ModelFavoritesManager.toggleFavorite(context, it)
                                        favorites = com.varcain.guitarrackcraft.engine.ModelFavoritesManager.getFavorites(context)
                                    },
                                    onDelete = {
                                        java.io.File(it).delete()
                                        refreshFiles()
                                    }
                                )
                            }
                        } else {
                            val isExpanded = expandedGroups.contains(groupName)
                            item(key = groupName) {
                                GroupRow(
                                    name = groupName.replace("_", " "),
                                    isExpanded = isExpanded,
                                    onClick = {
                                        expandedGroups = if (isExpanded) expandedGroups - groupName else expandedGroups + groupName
                                    }
                                )
                            }
                            if (isExpanded) {
                                items(files) { file ->
                                    FileItemRow(
                                        name = file.nameWithoutExtension,
                                        path = file.absolutePath,
                                        isFavorite = false,
                                        onSelect = { onFileSelected(it) },
                                        onToggleFavorite = {
                                            com.varcain.guitarrackcraft.engine.ModelFavoritesManager.toggleFavorite(context, it)
                                            favorites = com.varcain.guitarrackcraft.engine.ModelFavoritesManager.getFavorites(context)
                                        },
                                        onDelete = {
                                            java.io.File(it).delete()
                                            refreshFiles()
                                        },
                                        modifier = Modifier.padding(start = 24.dp)
                                    )
                                }
                            }
                        }
                    }
                }
            }
        },
        confirmButton = {},
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        }
    )
}

@Composable
private fun GroupRow(
    name: String,
    isExpanded: Boolean,
    onClick: () -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable { onClick() }
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Icon(
            Icons.Default.Folder,
            contentDescription = null,
            modifier = Modifier.size(24.dp).padding(4.dp),
            tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.7f)
        )
        Text(
            text = name,
            modifier = Modifier.weight(1f).padding(horizontal = 8.dp),
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.Bold
        )
        Icon(
            if (isExpanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
            contentDescription = null,
            modifier = Modifier.size(24.dp).padding(4.dp),
            tint = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

@Composable
private fun FileItemRow(
    name: String,
    path: String,
    isFavorite: Boolean,
    canDelete: Boolean = true,
    onSelect: (String) -> Unit,
    onToggleFavorite: (String) -> Unit,
    onDelete: ((String) -> Unit)? = null,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .clickable { onSelect(path) }
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        IconButton(
            onClick = { onToggleFavorite(path) },
            modifier = Modifier.size(32.dp)
        ) {
            Icon(
                if (isFavorite) Icons.Default.Favorite else Icons.Default.FavoriteBorder,
                contentDescription = "Favorite",
                tint = if (isFavorite) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(18.dp)
            )
        }
        Text(
            text = name,
            modifier = Modifier.weight(1f).padding(horizontal = 8.dp),
            style = MaterialTheme.typography.bodyMedium
        )
        if (canDelete && onDelete != null) {
            IconButton(
                onClick = { onDelete(path) },
                modifier = Modifier.size(32.dp)
            ) {
                Icon(
                    Icons.Default.Delete,
                    contentDescription = "Delete",
                    tint = MaterialTheme.colorScheme.error.copy(alpha = 0.7f),
                    modifier = Modifier.size(18.dp)
                )
            }
        }
    }
}

@Composable
private fun X11FilePickerDialog(
    config: X11FilePickerConfig,
    sourcePluginIndex: Int,
    sourcePropertyUri: String,
    onFileSelected: (String) -> Unit,
    onBrowseFiles: () -> Unit,
    onNavigateToTone3000: (String?, String?, String?, Int, String?) -> Unit,
    onDismiss: () -> Unit
) {
    GenericFilePickerDialog(
        title = config.title,
        storageDirs = config.storageDirs,
        extensions = config.extensions,
        builtInItems = config.builtInItems,
        onFileSelected = onFileSelected,
        onBrowseFiles = onBrowseFiles,
        onNavigateToTone3000 = {
            val initialPlatform = when {
                config.title.contains("Neural Model") -> "nam"
                config.title.contains("AIDA-X") -> "aida-x"
                config.title.contains("Impulse Response") -> "ir"
                else -> null
            }
            val initialTag = if (initialPlatform == "aida-x") "aida-x" else null
            val initialGear = if (config.title.contains("Impulse Response")) "ir" else null
            val sourceSlot = sourcePropertyUri.substringAfterLast('#', "").ifEmpty { null }
            onNavigateToTone3000(initialTag, initialGear, initialPlatform, sourcePluginIndex, sourceSlot)
        },
        onDismiss = onDismiss
    )
}

private fun extractModelsFromZip(
    context: android.content.Context,
    uri: android.net.Uri,
    zipFileName: String,
    config: ModelPluginConfig
): List<java.io.File> {
    val baseName = zipFileName.substringBeforeLast('.')
    val storageDir = config.storageDirs.first()
    val destDir = java.io.File(context.filesDir, "$storageDir/$baseName")
    destDir.mkdirs()

    val extractedFiles = mutableListOf<java.io.File>()
    context.contentResolver.openInputStream(uri)?.use { inputStream ->
        java.util.zip.ZipInputStream(inputStream).use { zis ->
            var entry = zis.nextEntry
            while (entry != null) {
                if (!entry.isDirectory) {
                    val entryName = entry.name.substringAfterLast('/')
                    val ext = entryName.substringAfterLast('.', "").lowercase()
                    if (ext in config.extensions) {
                        val outFile = java.io.File(destDir, entryName)
                        outFile.outputStream().use { out -> zis.copyTo(out) }
                        extractedFiles.add(outFile)
                    }
                }
                zis.closeEntry()
                entry = zis.nextEntry
            }
        }
    }
    return extractedFiles.sortedBy { it.name.lowercase() }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ModelPicker(
    pluginIndex: Int,
    viewModel: RackViewModel,
    config: ModelPluginConfig,
    externalPickerTrigger: Int = 0,
    onActiveModelChanged: ((String?) -> Unit)? = null,
    onModelFilesChanged: ((List<java.io.File>) -> Unit)? = null,
    onNavigateToTone3000: () -> Unit = {}
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var modelFiles by remember { mutableStateOf<List<java.io.File>>(emptyList()) }
    var selectedModelIndex by rememberSaveable { mutableStateOf(-1) }
    var activeModelName by rememberSaveable { mutableStateOf<String?>(null) }
    var isExtracting by remember { mutableStateOf(false) }
    var showDialog by remember { mutableStateOf(false) }

    // Report active model name changes to parent
    LaunchedEffect(activeModelName) {
        onActiveModelChanged?.invoke(activeModelName)
    }

    // Report model files changes to parent
    LaunchedEffect(modelFiles) {
        onModelFilesChanged?.invoke(modelFiles)
    }

    val launcher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult

        val fileName = uri.lastPathSegment?.substringAfterLast('/') ?: "model"

        if (fileName.lowercase().endsWith(".zip")) {
            isExtracting = true
            scope.launch(Dispatchers.IO) {
                try {
                    val models = extractModelsFromZip(context, uri, fileName, config)
                    withContext(Dispatchers.Main) {
                        isExtracting = false
                        if (models.isNotEmpty()) {
                            modelFiles = models
                            selectedModelIndex = 0
                            activeModelName = models[0].nameWithoutExtension
                            viewModel.setPluginFilePath(
                                pluginIndex,
                                config.propertyUri,
                                models[0].absolutePath
                            )
                        } else {
                            Log.e("ModelPicker", "No models found in $fileName")
                        }
                    }
                } catch (e: Exception) {
                    Log.e("ModelPicker", "Failed to extract zip: ${e.message}", e)
                    withContext(Dispatchers.Main) { isExtracting = false }
                }
            }
        } else {
            try {
                val storageDir = config.storageDirs.first()
                val modelsDir = java.io.File(context.filesDir, storageDir)
                modelsDir.mkdirs()
                val destFile = java.io.File(modelsDir, fileName)

                context.contentResolver.openInputStream(uri)?.use { input ->
                    destFile.outputStream().use { output -> input.copyTo(output) }
                }

                modelFiles = listOf(destFile)
                selectedModelIndex = 0
                activeModelName = destFile.nameWithoutExtension
                viewModel.setPluginFilePath(
                    pluginIndex,
                    config.propertyUri,
                    destFile.absolutePath
                )
            } catch (e: Exception) {
                Log.e("ModelPicker", "Failed to load model: ${e.message}", e)
            }
        }
    }

    // External trigger to open file picker (e.g. from modgui click)
    LaunchedEffect(externalPickerTrigger) {
        if (externalPickerTrigger > 0) {
            showDialog = true
        }
    }

    if (showDialog) {
        GenericFilePickerDialog(
            title = "Select Model",
            storageDirs = config.storageDirs,
            extensions = config.extensions,
            builtInItems = config.builtInItems,
            onFileSelected = { path ->
                showDialog = false
                val name = if (path == "default") {
                    config.builtInItems.find { it.second == "default" }?.first ?: "Default"
                } else {
                    java.io.File(path).nameWithoutExtension
                }
                activeModelName = name
                viewModel.setPluginFilePath(pluginIndex, config.propertyUri, path)
            },
            onBrowseFiles = {
                showDialog = false
                launcher.launch(arrayOf("*/*"))
            },
            onNavigateToTone3000 = onNavigateToTone3000,
            onDismiss = {
                showDialog = false
            }
        )
    }
}

@Composable
private fun RackBottomBar(
    isEngineRunning: Boolean,
    onToggleEngine: () -> Unit,
    onAddPlugin: () -> Unit,
    onOpenPresets: () -> Unit,
    isRecording: Boolean,
    recordingDurationSec: Double,
    onToggleRecording: () -> Unit,
    onOpenWav: () -> Unit
) {
    Surface(tonalElevation = 3.dp) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .navigationBarsPadding()
                .padding(vertical = 4.dp),
            horizontalArrangement = Arrangement.SpaceEvenly,
            verticalAlignment = Alignment.CenterVertically
        ) {
            BottomBarButton(
                icon = Icons.Default.LibraryMusic,
                label = "Presets",
                onClick = onOpenPresets
            )
            BottomBarButton(
                icon = Icons.Default.Add,
                label = "Add",
                onClick = onAddPlugin,
                testTag = "rack_add_plugin"
            )
            BottomBarButton(
                icon = if (isRecording) Icons.Default.Stop else Icons.Default.FiberManualRecord,
                label = if (isRecording) formatRecordingDuration(recordingDurationSec) else "Record",
                onClick = onToggleRecording,
                tint = if (isRecording) MaterialTheme.colorScheme.error else Color(0xFFE53935)
            )
            BottomBarButton(
                icon = Icons.Default.MusicNote,
                label = "Playback",
                onClick = onOpenWav
            )
            BottomBarButton(
                icon = if (isEngineRunning) Icons.Default.Stop else Icons.Default.PlayArrow,
                label = if (isEngineRunning) "Stop" else "Play",
                onClick = onToggleEngine,
                tint = if (isEngineRunning) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface,
                testTag = "rack_engine_fab"
            )
        }
    }
}

private fun formatRecordingDuration(seconds: Double): String {
    val totalSec = seconds.toInt()
    val min = totalSec / 60
    val sec = totalSec % 60
    return "%d:%02d".format(min, sec)
}

@Composable
private fun BottomBarButton(
    icon: ImageVector,
    label: String,
    onClick: () -> Unit,
    tint: Color = MaterialTheme.colorScheme.onSurface,
    testTag: String? = null
) {
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        modifier = Modifier
            .clickable(onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 4.dp)
            .then(if (testTag != null) Modifier.testTag(testTag) else Modifier)
    ) {
        Icon(icon, contentDescription = label, tint = tint, modifier = Modifier.size(24.dp))
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = tint
        )
    }
}

@Composable
private fun WavLoadDialog(
    isEngineRunning: Boolean,
    wavLoaded: Boolean,
    fileName: String?,
    positionSec: Double,
    durationSec: Double,
    isPlaying: Boolean,
    repeat: Boolean,
    onLoadWav: () -> Unit,
    onLoadRecordings: () -> Unit,
    onPlayPause: () -> Unit,
    onRestart: () -> Unit,
    onStop: () -> Unit,
    onToggleRepeat: () -> Unit,
    onSeek: (Double) -> Unit,
    onClose: () -> Unit,
    processEffects: Boolean,
    onToggleProcessEffects: () -> Unit,
    onDismiss: () -> Unit
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Playback") },
        text = {
            Column(modifier = Modifier.fillMaxWidth()) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Button(
                        onClick = onLoadWav,
                        enabled = isEngineRunning,
                        modifier = Modifier.weight(1f),
                        contentPadding = PaddingValues(horizontal = 12.dp, vertical = 8.dp)
                    ) {
                        Icon(Icons.Default.MusicNote, contentDescription = null, modifier = Modifier.size(18.dp))
                        Spacer(modifier = Modifier.width(4.dp))
                        Text("Load WAV", style = MaterialTheme.typography.labelMedium)
                    }
                    OutlinedButton(
                        onClick = onLoadRecordings,
                        enabled = isEngineRunning,
                        modifier = Modifier.weight(1f),
                        contentPadding = PaddingValues(horizontal = 12.dp, vertical = 8.dp)
                    ) {
                        Icon(Icons.Default.Album, contentDescription = null, modifier = Modifier.size(18.dp))
                        Spacer(modifier = Modifier.width(4.dp))
                        Text("Recordings", style = MaterialTheme.typography.labelMedium)
                    }
                }

                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.clickable(onClick = onToggleProcessEffects)
                        .padding(vertical = 4.dp)
                ) {
                    Checkbox(
                        checked = processEffects,
                        onCheckedChange = { onToggleProcessEffects() }
                    )
                    Text(
                        text = "Process through effects",
                        style = MaterialTheme.typography.bodyMedium
                    )
                }

                if (wavLoaded) {
                    Spacer(modifier = Modifier.height(12.dp))

                    // File name
                    fileName?.let { name ->
                        Text(
                            text = name,
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 1
                        )
                        Spacer(modifier = Modifier.height(8.dp))
                    }

                    // Seek slider
                    if (durationSec > 0) {
                        Slider(
                            value = positionSec.toFloat().coerceIn(0f, durationSec.toFloat()),
                            onValueChange = { onSeek(it.toDouble()) },
                            valueRange = 0f..durationSec.toFloat(),
                            modifier = Modifier.fillMaxWidth()
                        )
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween
                        ) {
                            Text(
                                text = formatWavTime(positionSec),
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                            Text(
                                text = formatWavTime(durationSec),
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    }

                    // Playback controls
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.Center,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        IconButton(onClick = onToggleRepeat, modifier = Modifier.size(36.dp)) {
                            Icon(
                                Icons.Default.Repeat,
                                contentDescription = if (repeat) "Repeat on" else "Repeat off",
                                tint = if (repeat) MaterialTheme.colorScheme.primary
                                       else MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.size(20.dp)
                            )
                        }
                        Spacer(modifier = Modifier.width(16.dp))
                        IconButton(onClick = onRestart, modifier = Modifier.size(36.dp)) {
                            Icon(
                                Icons.Default.SkipPrevious,
                                contentDescription = "Restart",
                                modifier = Modifier.size(24.dp)
                            )
                        }
                        Spacer(modifier = Modifier.width(8.dp))
                        IconButton(onClick = onPlayPause, modifier = Modifier.size(44.dp)) {
                            Icon(
                                if (isPlaying) Icons.Default.Pause else Icons.Default.PlayArrow,
                                contentDescription = if (isPlaying) "Pause" else "Play",
                                modifier = Modifier.size(32.dp)
                            )
                        }
                        Spacer(modifier = Modifier.width(8.dp))
                        IconButton(onClick = onStop, modifier = Modifier.size(36.dp)) {
                            Icon(
                                Icons.Default.StopCircle,
                                contentDescription = "Stop",
                                modifier = Modifier.size(24.dp)
                            )
                        }
                    }

                    // Unload button
                    Spacer(modifier = Modifier.height(8.dp))
                    TextButton(onClick = onClose) {
                        Icon(Icons.Default.Close, contentDescription = null, modifier = Modifier.size(16.dp))
                        Spacer(modifier = Modifier.width(4.dp))
                        Text("Unload")
                    }
                } else if (!isEngineRunning) {
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(
                        text = "Start the engine to load a WAV file",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        },
        confirmButton = {},
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Close") }
        }
    )
}

@Composable
private fun WavPlaybackBar(
    fileName: String?,
    positionSec: Double,
    durationSec: Double,
    isPlaying: Boolean,
    repeat: Boolean,
    onPlayPause: () -> Unit,
    onRestart: () -> Unit,
    onStop: () -> Unit,
    onToggleRepeat: () -> Unit,
    onSeek: (Double) -> Unit,
    onClose: () -> Unit
) {
    Surface(tonalElevation = 1.dp) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 12.dp, vertical = 4.dp)
        ) {
            // File name + close
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Icon(
                    Icons.Default.MusicNote,
                    contentDescription = null,
                    modifier = Modifier.size(16.dp),
                    tint = MaterialTheme.colorScheme.primary
                )
                Spacer(modifier = Modifier.width(4.dp))
                Text(
                    text = fileName ?: "WAV",
                    style = MaterialTheme.typography.bodySmall,
                    maxLines = 1,
                    modifier = Modifier.weight(1f)
                )
                IconButton(onClick = onClose, modifier = Modifier.size(24.dp)) {
                    Icon(Icons.Default.Close, contentDescription = "Close", modifier = Modifier.size(16.dp))
                }
            }

            // Progress slider
            if (durationSec > 0) {
                Slider(
                    value = positionSec.toFloat().coerceIn(0f, durationSec.toFloat()),
                    onValueChange = { onSeek(it.toDouble()) },
                    valueRange = 0f..durationSec.toFloat(),
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(24.dp)
                )
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Text(
                        text = formatWavTime(positionSec),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Text(
                        text = formatWavTime(durationSec),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }

            // Playback controls
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.Center,
                verticalAlignment = Alignment.CenterVertically
            ) {
                IconButton(onClick = onToggleRepeat, modifier = Modifier.size(36.dp)) {
                    Icon(
                        Icons.Default.Repeat,
                        contentDescription = if (repeat) "Repeat on" else "Repeat off",
                        tint = if (repeat) MaterialTheme.colorScheme.primary
                               else MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.size(20.dp)
                    )
                }
                Spacer(modifier = Modifier.width(16.dp))
                IconButton(onClick = onRestart, modifier = Modifier.size(36.dp)) {
                    Icon(
                        Icons.Default.SkipPrevious,
                        contentDescription = "Restart",
                        modifier = Modifier.size(24.dp)
                    )
                }
                Spacer(modifier = Modifier.width(8.dp))
                IconButton(onClick = onPlayPause, modifier = Modifier.size(44.dp)) {
                    Icon(
                        if (isPlaying) Icons.Default.Pause else Icons.Default.PlayArrow,
                        contentDescription = if (isPlaying) "Pause" else "Play",
                        modifier = Modifier.size(32.dp)
                    )
                }
                Spacer(modifier = Modifier.width(8.dp))
                IconButton(onClick = onStop, modifier = Modifier.size(36.dp)) {
                    Icon(
                        Icons.Default.StopCircle,
                        contentDescription = "Stop",
                        modifier = Modifier.size(24.dp)
                    )
                }
            }
        }
    }
}

private fun formatWavTime(sec: Double): String {
    val totalSec = sec.coerceAtLeast(0.0).toInt()
    val m = totalSec / 60
    val s = totalSec % 60
    return "%d:%02d".format(m, s)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun PresetBottomSheet(
    viewModel: RackViewModel,
    onDismiss: () -> Unit
) {
    val context = LocalContext.current
    val presetList by viewModel.presetList.collectAsState()
    val recentPresets by viewModel.recentPresets.collectAsState()
    var showSaveDialog by remember { mutableStateOf(false) }

    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp)
                .padding(bottom = 24.dp)
        ) {
            // Recently used section
            if (recentPresets.isNotEmpty()) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text("Recently Used", style = MaterialTheme.typography.titleSmall)
                    TextButton(onClick = {
                        viewModel.clearRecentPresets(context)
                    }) {
                        Text("Clear")
                    }
                }
                recentPresets.forEach { name ->
                    PresetListItem(
                        name = name,
                        onLoad = {
                            viewModel.loadPreset(context, name)
                            onDismiss()
                        },
                        onShare = {
                            val json = viewModel.getPresetJson(context, name)
                            if (json != null) {
                                val intent = Intent(Intent.ACTION_SEND).apply {
                                    type = "text/plain"
                                    putExtra(Intent.EXTRA_TEXT, json)
                                    putExtra(Intent.EXTRA_SUBJECT, "$name.json")
                                }
                                context.startActivity(Intent.createChooser(intent, "Share preset"))
                            }
                        },
                        showDelete = false
                    )
                }
                Divider(modifier = Modifier.padding(vertical = 8.dp))
            }

            // All presets section
            Text("All Presets", style = MaterialTheme.typography.titleSmall)
            if (presetList.isEmpty()) {
                Text(
                    "No saved presets",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(vertical = 12.dp)
                )
            } else {
                presetList.forEach { name ->
                    PresetListItem(
                        name = name,
                        onLoad = {
                            viewModel.loadPreset(context, name)
                            onDismiss()
                        },
                        onShare = {
                            val json = viewModel.getPresetJson(context, name)
                            if (json != null) {
                                val intent = Intent(Intent.ACTION_SEND).apply {
                                    type = "text/plain"
                                    putExtra(Intent.EXTRA_TEXT, json)
                                    putExtra(Intent.EXTRA_SUBJECT, "$name.json")
                                }
                                context.startActivity(Intent.createChooser(intent, "Share preset"))
                            }
                        },
                        onDelete = {
                            viewModel.deletePreset(context, name)
                        },
                        showDelete = true
                    )
                }
            }

            Spacer(modifier = Modifier.height(12.dp))
            Button(
                onClick = { showSaveDialog = true },
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("Save Current Preset")
            }
        }
    }

    if (showSaveDialog) {
        PresetSaveDialog(
            existingNames = presetList,
            onSave = { name ->
                showSaveDialog = false
                viewModel.savePreset(context, name)
            },
            onDismiss = { showSaveDialog = false }
        )
    }
}

@Composable
private fun PresetListItem(
    name: String,
    onLoad: () -> Unit,
    onShare: () -> Unit,
    onDelete: (() -> Unit)? = null,
    showDelete: Boolean = false
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onLoad)
            .padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = name,
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.weight(1f)
        )
        IconButton(onClick = onShare, modifier = Modifier.size(32.dp)) {
            Icon(Icons.Default.Share, contentDescription = "Share", modifier = Modifier.size(18.dp))
        }
        if (showDelete && onDelete != null) {
            IconButton(onClick = onDelete, modifier = Modifier.size(32.dp)) {
                Icon(Icons.Default.Delete, contentDescription = "Delete", modifier = Modifier.size(18.dp))
            }
        }
    }
}

@Composable
private fun PresetSaveDialog(
    existingNames: List<String>,
    onSave: (String) -> Unit,
    onDismiss: () -> Unit
) {
    var name by remember { mutableStateOf("") }
    val nameExists = name.isNotBlank() && existingNames.any { it.equals(name.trim(), ignoreCase = true) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Save Preset") },
        text = {
            Column {
                OutlinedTextField(
                    value = name,
                    onValueChange = { name = it },
                    label = { Text("Preset name") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth()
                )
                if (nameExists) {
                    Text(
                        text = "A preset with this name exists and will be overwritten",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                        modifier = Modifier.padding(top = 4.dp)
                    )
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = { onSave(name.trim()) },
                enabled = name.isNotBlank()
            ) {
                Text(if (nameExists) "Overwrite" else "Save")
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        }
    )
}

@Composable
private fun RecordingPickerDialog(
    onPickRecording: (path: String, isRaw: Boolean) -> Unit,
    onDismiss: () -> Unit
) {
    val context = LocalContext.current
    val recordings = remember { RecordingManager.listRecordings(context) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Select Recording") },
        text = {
            if (recordings.isEmpty()) {
                Text(
                    "No recordings available.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            } else {
                LazyColumn(
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                    modifier = Modifier.heightIn(max = 400.dp)
                ) {
                    items(recordings, key = { it.timestamp }) { entry ->
                        Card(modifier = Modifier.fillMaxWidth()) {
                            Column(modifier = Modifier.padding(12.dp)) {
                                Text(
                                    text = entry.displayName,
                                    style = MaterialTheme.typography.titleSmall,
                                    fontWeight = FontWeight.Medium
                                )
                                Text(
                                    text = formatWavTime(entry.durationSec),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                                Spacer(modifier = Modifier.height(4.dp))
                                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                    OutlinedButton(
                                        onClick = { onPickRecording(entry.rawFile.absolutePath, true) },
                                        modifier = Modifier.weight(1f),
                                        contentPadding = PaddingValues(horizontal = 8.dp, vertical = 4.dp)
                                    ) {
                                        Text("Raw", style = MaterialTheme.typography.labelSmall)
                                    }
                                    OutlinedButton(
                                        onClick = { onPickRecording(entry.processedFile.absolutePath, false) },
                                        modifier = Modifier.weight(1f),
                                        contentPadding = PaddingValues(horizontal = 8.dp, vertical = 4.dp)
                                    ) {
                                        Text("Processed", style = MaterialTheme.typography.labelSmall)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        },
        confirmButton = {},
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Close") }
        }
    )
}

@Composable
fun AboutDialog(
    onDismiss: () -> Unit
) {
    val context = LocalContext.current
    val appName = remember(context) {
        context.applicationInfo.loadLabel(context.packageManager).toString()
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("About") },
        text = {
            Column(
                verticalArrangement = Arrangement.spacedBy(12.dp),
                modifier = Modifier.padding(vertical = 8.dp)
            ) {
                Text(
                    text = "$appName ${BuildConfig.VERSION_NAME}",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold
                )
                Text(
                    text = "Build: ${BuildConfig.BUILD_DATE} ${BuildConfig.BUILD_TIME} · ${BuildConfig.BUILD_HOST}",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Text(
                    text = "© \"Varcain\" Kamil Lulko",
                    style = MaterialTheme.typography.bodyMedium
                )
                Text(
                    text = "This app is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License (GPL).",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text("OK") }
        }
    )
}
