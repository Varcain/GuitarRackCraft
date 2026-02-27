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

package com.varcain.guitarrackcraft.ui.tone3000

import android.content.Intent
import android.net.Uri
import android.widget.Toast
import java.net.URLEncoder
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.ExitToApp
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.FilterList
import androidx.compose.material.icons.filled.MusicNote
import androidx.compose.material.icons.filled.Person
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Sort
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import coil.compose.AsyncImage
import coil.request.ImageRequest

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun Tone3000Screen(
    onNavigateBack: () -> Unit,
    onNavigateToDetail: (Tone) -> Unit = {},
    initialTag: String? = null,
    initialGear: String? = null,
    initialPlatform: String? = null,
    sourcePluginIndex: Int = -1,
    sourceSlot: String? = null,
    viewModel: Tone3000ViewModel = viewModel()
) {
    val tones by viewModel.tones.collectAsState()
    val isLoading by viewModel.isLoading.collectAsState()
    val error by viewModel.error.collectAsState()
    val isAuthenticated by viewModel.isAuthenticated.collectAsState()
    val user by viewModel.user.collectAsState()
    val context = LocalContext.current
    val snackbarHostState = remember { SnackbarHostState() }

    val selectedGear by viewModel.selectedGear.collectAsState()
    val selectedPlatform by viewModel.selectedPlatform.collectAsState()
    val selectedTags by viewModel.selectedTags.collectAsState()
    val selectedSizes by viewModel.selectedSizes.collectAsState()
    val isCalibrated by viewModel.isCalibrated.collectAsState()
    val selectedSort by viewModel.selectedSort.collectAsState()
    val modelsForTone by viewModel.modelsForTone.collectAsState()
    val downloadedIds by viewModel.downloadedModelIds.collectAsState()
    val hasSourcePlugin = sourcePluginIndex >= 0
    val filesDir = LocalContext.current.filesDir

    var showFilterSheet by remember { mutableStateOf(false) }
    var showSortMenu by remember { mutableStateOf(false) }

    LaunchedEffect(sourcePluginIndex, sourceSlot) {
        viewModel.setSourcePlugin(sourcePluginIndex, sourceSlot)
    }

    LaunchedEffect(initialTag, initialGear, initialPlatform) {
        viewModel.initFilters(initialTag, initialGear, initialPlatform)
    }

    LaunchedEffect(Unit) {
        viewModel.downloadStatus.collect { status ->
            snackbarHostState.showSnackbar(status)
        }
    }

    LaunchedEffect(Unit) {
        viewModel.toastMessage.collect { msg ->
            Toast.makeText(context, msg, Toast.LENGTH_LONG).show()
        }
    }

    var searchQuery by remember { mutableStateOf("") }

    val activeFilterCount = listOfNotNull(
        selectedGear,
        selectedPlatform,
        isCalibrated
    ).size + selectedTags.size + selectedSizes.size

    Scaffold(
        snackbarHost = { SnackbarHost(snackbarHostState) },
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text("TONE3000", fontWeight = FontWeight.Bold)
                        if (isAuthenticated) {
                            Text(
                                text = user?.username ?: "Authenticated",
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.primary
                            )
                        }
                    }
                },
                navigationIcon = {
                    IconButton(onClick = onNavigateBack) {
                        Icon(Icons.Default.ArrowBack, contentDescription = "Back")
                    }
                },
                actions = {
                    if (isAuthenticated) {
                        IconButton(onClick = { viewModel.logout() }) {
                            Icon(
                                Icons.Default.ExitToApp,
                                contentDescription = "Logout",
                                tint = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    } else {
                        FilledTonalButton(
                            onClick = {
                                val redirectUrl = URLEncoder.encode("guitarrackcraft://tone3000auth", "UTF-8")
                                val url = "https://www.tone3000.com/api/v1/auth?redirect_url=$redirectUrl"
                                val intent = Intent(Intent.ACTION_VIEW, Uri.parse(url))
                                context.startActivity(intent)
                            },
                            modifier = Modifier.padding(end = 8.dp)
                        ) {
                            Icon(Icons.Default.Person, contentDescription = null, modifier = Modifier.size(16.dp))
                            Spacer(modifier = Modifier.width(4.dp))
                            Text("Login")
                        }
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface
                )
            )
        }
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
        ) {
            // Search Bar Row
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 12.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                TextField(
                    value = searchQuery,
                    onValueChange = {
                        searchQuery = it
                        viewModel.searchTones(it)
                    },
                    modifier = Modifier.weight(1f),
                    placeholder = { Text("Search tones...") },
                    leadingIcon = {
                        Icon(
                            Icons.Default.Search,
                            contentDescription = null,
                            tint = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    },
                    singleLine = true,
                    shape = RoundedCornerShape(24.dp),
                    colors = TextFieldDefaults.colors(
                        focusedContainerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f),
                        unfocusedContainerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f),
                        focusedIndicatorColor = Color.Transparent,
                        unfocusedIndicatorColor = Color.Transparent
                    )
                )

                // Filter button with badge
                Box {
                    IconButton(onClick = { showFilterSheet = true }) {
                        Icon(
                            Icons.Default.FilterList,
                            contentDescription = "Filters",
                            tint = if (activeFilterCount > 0) MaterialTheme.colorScheme.primary
                                   else MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                    if (activeFilterCount > 0) {
                        Badge(
                            modifier = Modifier.align(Alignment.TopEnd).offset(x = (-4).dp, y = 4.dp)
                        ) {
                            Text("$activeFilterCount")
                        }
                    }
                }

                // Sort button
                Box {
                    IconButton(onClick = { showSortMenu = true }) {
                        Icon(
                            Icons.Default.Sort,
                            contentDescription = "Sort",
                            tint = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                    DropdownMenu(
                        expanded = showSortMenu,
                        onDismissRequest = { showSortMenu = false }
                    ) {
                        TonesSort.values().forEach { sort ->
                            DropdownMenuItem(
                                text = {
                                    Text(
                                        text = sort.displayName,
                                        fontWeight = if (selectedSort == sort) FontWeight.Bold else FontWeight.Normal
                                    )
                                },
                                onClick = {
                                    showSortMenu = false
                                    viewModel.setSort(sort)
                                },
                                leadingIcon = {
                                    if (selectedSort == sort) {
                                        Icon(Icons.Default.Check, contentDescription = null, modifier = Modifier.size(18.dp))
                                    }
                                }
                            )
                        }
                    }
                }
            }

            // Content area
            if (!isAuthenticated && tones.isEmpty() && !isLoading) {
                // Login prompt
                Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Column(
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.spacedBy(16.dp),
                        modifier = Modifier.padding(32.dp)
                    ) {
                        Surface(
                            shape = CircleShape,
                            color = MaterialTheme.colorScheme.primaryContainer,
                            modifier = Modifier.size(80.dp)
                        ) {
                            Box(contentAlignment = Alignment.Center) {
                                Icon(
                                    Icons.Default.MusicNote,
                                    contentDescription = null,
                                    modifier = Modifier.size(40.dp),
                                    tint = MaterialTheme.colorScheme.onPrimaryContainer
                                )
                            }
                        }
                        Text(
                            text = "Browse thousands of tones",
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.Bold
                        )
                        Text(
                            text = "Sign in with your TONE3000 account to discover and download amp models, IRs, and more.",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.widthIn(max = 280.dp)
                        )
                        Spacer(modifier = Modifier.height(8.dp))
                        Button(
                            onClick = {
                                val redirectUrl = URLEncoder.encode("guitarrackcraft://tone3000auth", "UTF-8")
                                val url = "https://www.tone3000.com/api/v1/auth?redirect_url=$redirectUrl"
                                val intent = Intent(Intent.ACTION_VIEW, Uri.parse(url))
                                context.startActivity(intent)
                            },
                            modifier = Modifier.fillMaxWidth(0.7f),
                            shape = RoundedCornerShape(12.dp)
                        ) {
                            Icon(Icons.Default.Person, contentDescription = null, modifier = Modifier.size(18.dp))
                            Spacer(modifier = Modifier.width(8.dp))
                            Text("Login with TONE3000")
                        }
                    }
                }
            } else if (error != null && tones.isEmpty()) {
                // Error state
                Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Column(
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.spacedBy(12.dp),
                        modifier = Modifier.padding(32.dp)
                    ) {
                        Text(
                            text = error ?: "Something went wrong",
                            color = MaterialTheme.colorScheme.error,
                            style = MaterialTheme.typography.bodyMedium
                        )
                        FilledTonalButton(onClick = { viewModel.searchTones(searchQuery) }) {
                            Text("Retry")
                        }
                    }
                }
            } else {
                // Tone list
                val listState = rememberLazyListState()

                val shouldLoadMore = remember {
                    derivedStateOf {
                        val lastVisibleItem = listState.layoutInfo.visibleItemsInfo.lastOrNull()
                        lastVisibleItem != null && lastVisibleItem.index >= tones.size - 5
                    }
                }

                LaunchedEffect(shouldLoadMore.value) {
                    if (shouldLoadMore.value) {
                        viewModel.loadNextPage()
                    }
                }

                LazyColumn(
                    state = listState,
                    modifier = Modifier.fillMaxSize(),
                    contentPadding = PaddingValues(horizontal = 12.dp, vertical = 8.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp)
                ) {
                    items(items = tones) { tone ->
                        ToneItem(
                            tone = tone,
                            onDownload = { viewModel.requestModelList(tone) },
                            onClick = { onNavigateToDetail(tone) }
                        )
                    }

                    if (isLoading) {
                        item {
                            Box(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(24.dp),
                                contentAlignment = Alignment.Center
                            ) {
                                CircularProgressIndicator(
                                    modifier = Modifier.size(32.dp),
                                    strokeWidth = 3.dp
                                )
                            }
                        }
                    }
                }
            }
        }
    }

    // Filter bottom sheet
    if (showFilterSheet) {
        ModalBottomSheet(
            onDismissRequest = { showFilterSheet = false },
            containerColor = MaterialTheme.colorScheme.surface
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 20.dp)
                    .padding(bottom = 32.dp)
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text("Filters", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
                    if (activeFilterCount > 0) {
                        TextButton(onClick = { viewModel.clearFilters() }) {
                            Text("Clear All")
                        }
                    }
                }

                Spacer(modifier = Modifier.height(20.dp))

                FilterSection(title = "Gear") {
                    FlowRow(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        listOf(
                            "amp" to "Amp Head",
                            "full-rig" to "Full Rig / Combo",
                            "pedal" to "Pedal",
                            "outboard" to "Outboard",
                            "ir" to "Impulse Response"
                        ).forEach { (value, label) ->
                            FilterChip(
                                selected = selectedGear == value,
                                onClick = { viewModel.setGearFilter(if (selectedGear == value) null else value) },
                                label = { Text(label) }
                            )
                        }
                    }
                }

                FilterSection(title = "Platform") {
                    FlowRow(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        Platform.values().forEach { platform ->
                            FilterChip(
                                selected = selectedPlatform == platform,
                                onClick = { viewModel.setPlatformFilter(if (selectedPlatform == platform) null else platform) },
                                label = { Text(platform.displayName) }
                            )
                        }
                    }
                }

                FilterSection(title = "Tags") {
                    FlowRow(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        listOf("nam", "aida-x", "ir", "metal", "high-gain", "rock", "crunch", "distortion", "clean").forEach { tag ->
                            FilterChip(
                                selected = selectedTags.contains(tag),
                                onClick = { viewModel.toggleTag(tag) },
                                label = { Text(tag) }
                            )
                        }
                    }
                }

                FilterSection(title = "Size / Architecture") {
                    FlowRow(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        listOf("standard", "lite", "feather", "nano", "custom").forEach { size ->
                            FilterChip(
                                selected = selectedSizes.contains(size),
                                onClick = { viewModel.toggleSize(size) },
                                label = { Text(ModelSize.fromString(size)) }
                            )
                        }
                    }
                }

                FilterSection(title = "Calibrated") {
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        FilterChip(
                            selected = isCalibrated == true,
                            onClick = { viewModel.setCalibrated(if (isCalibrated == true) null else true) },
                            label = { Text("Yes") }
                        )
                        FilterChip(
                            selected = isCalibrated == false,
                            onClick = { viewModel.setCalibrated(if (isCalibrated == false) null else false) },
                            label = { Text("No") }
                        )
                    }
                }
            }
        }
    }

    // Model selection bottom sheet
    modelsForTone?.let { (tone, models) ->
        ModalBottomSheet(
            onDismissRequest = { viewModel.dismissModelDialog() },
            containerColor = MaterialTheme.colorScheme.surface
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 20.dp)
                    .padding(bottom = 32.dp)
            ) {
                Text(
                    text = "Select Model",
                    style = MaterialTheme.typography.titleLarge,
                    fontWeight = FontWeight.Bold
                )
                Text(
                    text = tone.title,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Spacer(modifier = Modifier.height(16.dp))

                LazyColumn(
                    modifier = Modifier.heightIn(max = 400.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    items(models) { model ->
                        val isDownloaded = downloadedIds.contains(model.id) ||
                                ToneFileUtils.isModelDownloaded(filesDir, tone, model)
                        ModelSelectionItem(
                            model = model,
                            isDownloaded = isDownloaded,
                            hasSourcePlugin = hasSourcePlugin,
                            onSelect = {
                                if (isDownloaded && hasSourcePlugin) {
                                    viewModel.loadModelToPlugin(tone, model)
                                    viewModel.dismissModelDialog()
                                } else if (!isDownloaded) {
                                    viewModel.downloadTone(tone, model)
                                }
                            }
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun FilterSection(title: String, content: @Composable () -> Unit) {
    Column(modifier = Modifier.padding(bottom = 16.dp)) {
        Text(
            text = title,
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.primary,
            modifier = Modifier.padding(bottom = 8.dp)
        )
        content()
    }
}

@Composable
private fun ModelSelectionItem(
    model: Model,
    isDownloaded: Boolean = false,
    hasSourcePlugin: Boolean = false,
    onSelect: () -> Unit
) {
    val isClickable = !isDownloaded || hasSourcePlugin
    Card(
        modifier = Modifier.fillMaxWidth().clickable(enabled = isClickable, onClick = onSelect),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f)
        ),
        shape = RoundedCornerShape(12.dp)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = model.name,
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.SemiBold
                )
                Spacer(modifier = Modifier.height(2.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    ToneBadge(
                        text = ModelSize.fromString(model.size),
                        containerColor = MaterialTheme.colorScheme.secondaryContainer,
                        contentColor = MaterialTheme.colorScheme.onSecondaryContainer
                    )
                    model.platform?.let {
                        ToneBadge(
                            text = Platform.fromString(it),
                            containerColor = MaterialTheme.colorScheme.tertiaryContainer,
                            contentColor = MaterialTheme.colorScheme.onTertiaryContainer
                        )
                    }
                }
            }
            when {
                isDownloaded && hasSourcePlugin -> {
                    FilledIconButton(
                        onClick = onSelect,
                        modifier = Modifier.size(36.dp),
                        colors = IconButtonDefaults.filledIconButtonColors(
                            containerColor = Color(0xFF4CAF50)
                        )
                    ) {
                        Icon(
                            Icons.Default.PlayArrow,
                            contentDescription = "Load",
                            modifier = Modifier.size(18.dp)
                        )
                    }
                }
                isDownloaded -> {
                    FilledIconButton(
                        onClick = {},
                        enabled = false,
                        modifier = Modifier.size(36.dp),
                        colors = IconButtonDefaults.filledIconButtonColors(
                            containerColor = MaterialTheme.colorScheme.surfaceVariant,
                            disabledContainerColor = MaterialTheme.colorScheme.surfaceVariant
                        )
                    ) {
                        Icon(
                            Icons.Default.Check,
                            contentDescription = "Downloaded",
                            modifier = Modifier.size(18.dp),
                            tint = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
                else -> {
                    FilledIconButton(
                        onClick = onSelect,
                        modifier = Modifier.size(36.dp),
                        colors = IconButtonDefaults.filledIconButtonColors(
                            containerColor = MaterialTheme.colorScheme.primary
                        )
                    ) {
                        Icon(
                            Icons.Default.Download,
                            contentDescription = "Download",
                            modifier = Modifier.size(18.dp)
                        )
                    }
                }
            }
        }
    }
}

@Composable
fun ToneItem(tone: Tone, onDownload: () -> Unit, onClick: () -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth().clickable(onClick = onClick),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f)
        )
    ) {
        Row(
            modifier = Modifier
                .padding(10.dp)
                .height(IntrinsicSize.Min),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Thumbnail
            val imageUrl = tone.images?.firstOrNull() ?: tone.user?.avatar_url
            Box(
                modifier = Modifier
                    .size(88.dp)
                    .clip(RoundedCornerShape(10.dp))
                    .background(MaterialTheme.colorScheme.surfaceVariant)
            ) {
                AsyncImage(
                    model = ImageRequest.Builder(LocalContext.current)
                        .data(imageUrl)
                        .size(256, 256)
                        .crossfade(true)
                        .build(),
                    contentDescription = null,
                    modifier = Modifier.fillMaxSize(),
                    contentScale = ContentScale.Crop
                )
                // Platform badge overlay
                tone.platform?.let { platform ->
                    Surface(
                        color = MaterialTheme.colorScheme.inverseSurface.copy(alpha = 0.85f),
                        shape = RoundedCornerShape(bottomStart = 10.dp, topEnd = 0.dp),
                        modifier = Modifier.align(Alignment.TopEnd)
                    ) {
                        Text(
                            text = Platform.fromString(platform),
                            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                            style = MaterialTheme.typography.labelSmall,
                            fontWeight = FontWeight.Bold,
                            color = MaterialTheme.colorScheme.inverseOnSurface
                        )
                    }
                }
            }

            Spacer(modifier = Modifier.width(12.dp))

            // Info
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                Text(
                    text = tone.title,
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
                Text(
                    text = tone.user?.username ?: "Unknown",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1
                )

                Spacer(modifier = Modifier.weight(1f))

                // Tags row
                Row(
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    tone.gear?.let { gear ->
                        ToneBadge(
                            text = Gear.fromString(gear),
                            containerColor = MaterialTheme.colorScheme.secondaryContainer,
                            contentColor = MaterialTheme.colorScheme.onSecondaryContainer
                        )
                    }
                    Text(
                        text = "${tone.models_count} models",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }

            // Download button
            FilledIconButton(
                onClick = onDownload,
                modifier = Modifier.size(40.dp),
                colors = IconButtonDefaults.filledIconButtonColors(
                    containerColor = MaterialTheme.colorScheme.primaryContainer
                )
            ) {
                Icon(
                    Icons.Default.Download,
                    contentDescription = "Download",
                    modifier = Modifier.size(20.dp),
                    tint = MaterialTheme.colorScheme.onPrimaryContainer
                )
            }
        }
    }
}

@Composable
fun ToneBadge(text: String, containerColor: Color, contentColor: Color) {
    Surface(
        color = containerColor,
        shape = RoundedCornerShape(6.dp)
    ) {
        Text(
            text = text,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.Medium,
            color = contentColor
        )
    }
}
