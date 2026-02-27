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

package com.varcain.guitarrackcraft.ui.browser

import android.graphics.BitmapFactory
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.Image
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Star
import androidx.compose.material.icons.outlined.Star
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.varcain.guitarrackcraft.engine.PluginInfo
import com.varcain.guitarrackcraft.engine.UiType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PluginBrowserScreen(
    replaceIndex: Int = -1,
    onNavigateBack: () -> Unit,
    viewModel: PluginBrowserViewModel = viewModel()
) {
    val groupedPlugins by viewModel.groupedPlugins.collectAsState()
    val expandedAuthors by viewModel.expandedAuthors.collectAsState()
    val expandedCategories by viewModel.expandedCategories.collectAsState()
    val isLoading by viewModel.isLoading.collectAsState()
    val errorMessage by viewModel.errorMessage.collectAsState()
    val addFailureMessage by viewModel.addFailureMessage.collectAsState()
    val favorites by viewModel.favorites.collectAsState()

    val snackbarHostState = remember { SnackbarHostState() }

    // Reload plugins every time the screen is displayed (e.g. engine may have
    // been initialized after permission grant, or user returned to this screen)
    LaunchedEffect(Unit) {
        viewModel.refresh()
    }

    // Show Snackbar when add-to-rack fails (plugin binary missing, etc.)
    LaunchedEffect(addFailureMessage) {
        addFailureMessage?.let { msg ->
            snackbarHostState.showSnackbar(
                message = msg,
                duration = SnackbarDuration.Long
            )
            viewModel.clearAddFailureMessage()
        }
    }

    Scaffold(
        snackbarHost = { SnackbarHost(snackbarHostState) },
        topBar = {
            TopAppBar(
                title = { Text("Plugin Browser") },
                navigationIcon = {
                    IconButton(onClick = onNavigateBack) {
                        Icon(Icons.Default.ArrowBack, contentDescription = "Back")
                    }
                },
                actions = {
                    IconButton(onClick = { viewModel.refresh() }) {
                        Icon(Icons.Default.Refresh, contentDescription = "Refresh")
                    }
                }
            )
        }
    ) { padding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
        ) {
            when {
                isLoading -> {
                    CircularProgressIndicator(
                        modifier = Modifier.align(Alignment.Center)
                    )
                }
                errorMessage != null -> {
                    Column(
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(16.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.Center
                    ) {
                        Text(
                            text = errorMessage ?: "Unknown error",
                            color = MaterialTheme.colorScheme.error
                        )
                        Spacer(modifier = Modifier.height(16.dp))
                        Button(onClick = { viewModel.refresh() }) {
                            Text("Retry")
                        }
                    }
                }
                groupedPlugins.isEmpty() -> {
                    Column(
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(16.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.Center
                    ) {
                        Text("No plugins available")
                        Spacer(modifier = Modifier.height(8.dp))
                        Text(
                            text = "LV2 plugins are loaded from the app's extracted assets (assets/lv2). This app ships with GxPlugins in assets—if you see nothing here, the native build may be using the LV2 stub (no lilv/serd/sord). Build the LV2 libraries and place them in app/src/main/cpp/libs/lv2/, then rebuild the app. See LV2_INTEGRATION.md.",
                            style = MaterialTheme.typography.bodySmall
                        )
                    }
                }
                else -> {
                    LazyColumn(
                        modifier = Modifier.fillMaxSize(),
                        contentPadding = PaddingValues(16.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        groupedPlugins.forEach { authorGroup ->
                            // Author header
                            item(key = "author_${authorGroup.name}") {
                                AuthorHeader(
                                    authorName = authorGroup.name,
                                    pluginCount = authorGroup.categories.sumOf { it.plugins.size },
                                    isExpanded = expandedAuthors.contains(authorGroup.name),
                                    onToggle = { viewModel.toggleAuthor(authorGroup.name) }
                                )
                            }
                            
                            // Categories (only show if author is expanded)
                            if (expandedAuthors.contains(authorGroup.name)) {
                                authorGroup.categories.forEach { category ->
                                    val categoryKey = "${authorGroup.name}|${category.name}"
                                    val isCategoryExpanded = expandedCategories.contains(categoryKey)
                                    
                                    // Category header
                                    item(key = "category_${categoryKey}") {
                                        CategoryHeader(
                                            categoryName = category.name,
                                            pluginCount = category.plugins.size,
                                            isExpanded = isCategoryExpanded,
                                            onToggle = { 
                                                viewModel.toggleCategory(authorGroup.name, category.name) 
                                            }
                                        )
                                    }
                                    
                                    // Plugins in category (only show if category is expanded)
                                    if (isCategoryExpanded) {
                                        items(
                                            items = category.plugins,
                                            key = { "${authorGroup.name}_${category.name}_${it.fullId}" }
                                        ) { plugin ->
                                            PluginItem(
                                                plugin = plugin,
                                                isFavorite = favorites.contains(plugin.fullId),
                                                onToggleFavorite = { viewModel.toggleFavorite(plugin.fullId) },
                                                onClick = {
                                                    val success = if (replaceIndex >= 0) {
                                                        viewModel.replacePluginInRack(replaceIndex, plugin)
                                                    } else {
                                                        viewModel.addPluginToRack(plugin)
                                                    }
                                                    if (success) onNavigateBack()
                                                }
                                            )
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun AuthorHeader(
    authorName: String,
    pluginCount: Int,
    isExpanded: Boolean,
    onToggle: () -> Unit
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onToggle),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.primaryContainer
        ),
        elevation = CardDefaults.cardElevation(defaultElevation = 4.dp)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Icon(
                    imageVector = if (isExpanded) Icons.Default.KeyboardArrowDown else Icons.Default.KeyboardArrowRight,
                    contentDescription = if (isExpanded) "Collapse" else "Expand",
                    tint = MaterialTheme.colorScheme.onPrimaryContainer
                )
                Text(
                    text = authorName,
                    style = MaterialTheme.typography.titleLarge,
                    fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.onPrimaryContainer
                )
            }
            Text(
                text = "$pluginCount plugins",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onPrimaryContainer.copy(alpha = 0.7f)
            )
        }
    }
}

@Composable
fun CategoryHeader(
    categoryName: String,
    pluginCount: Int,
    isExpanded: Boolean,
    onToggle: () -> Unit
) {
    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = 16.dp)
            .clickable(onClick = onToggle),
        color = MaterialTheme.colorScheme.secondaryContainer.copy(alpha = 0.5f),
        shape = MaterialTheme.shapes.small
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 12.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                Icon(
                    imageVector = if (isExpanded) Icons.Default.KeyboardArrowDown else Icons.Default.KeyboardArrowRight,
                    contentDescription = if (isExpanded) "Collapse" else "Expand",
                    tint = MaterialTheme.colorScheme.onSecondaryContainer,
                    modifier = Modifier.size(20.dp)
                )
                Text(
                    text = categoryName,
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.SemiBold,
                    color = MaterialTheme.colorScheme.onSecondaryContainer
                )
            }
            Text(
                text = "$pluginCount",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSecondaryContainer.copy(alpha = 0.7f)
            )
        }
    }
}

@Composable
fun PluginThumbnail(
    thumbnailPath: String,
    modifier: Modifier = Modifier
) {
    val context = LocalContext.current
    
    val bitmap = produceState<androidx.compose.ui.graphics.ImageBitmap?>(initialValue = null, thumbnailPath) {
        value = withContext(Dispatchers.IO) {
            try {
                if (thumbnailPath.isNotEmpty()) {
                    context.assets.open("lv2/$thumbnailPath").use { inputStream ->
                        BitmapFactory.decodeStream(inputStream)?.asImageBitmap()
                    }
                } else {
                    null
                }
            } catch (e: Exception) {
                null
            }
        }
    }
    
    Box(
        modifier = modifier,
        contentAlignment = Alignment.Center
    ) {
        bitmap.value?.let { imageBitmap ->
            Image(
                bitmap = imageBitmap,
                contentDescription = "Plugin thumbnail",
                modifier = Modifier.fillMaxSize()
            )
        } ?: run {
            // Placeholder when no thumbnail
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(4.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = "🎸",
                    style = MaterialTheme.typography.titleMedium
                )
            }
        }
    }
}

@Composable
fun PluginItem(
    plugin: PluginInfo,
    isFavorite: Boolean = false,
    onToggleFavorite: () -> Unit = {},
    onClick: () -> Unit
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = 32.dp)
            .clickable(onClick = onClick)
            .testTag("browser_plugin_item"),
        elevation = CardDefaults.cardElevation(defaultElevation = 2.dp)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            // Thumbnail
            PluginThumbnail(
                thumbnailPath = plugin.thumbnailPath,
                modifier = Modifier
                    .size(80.dp)
            )

            // Plugin info
            Column(
                modifier = Modifier.weight(1f)
            ) {
                Text(
                    text = plugin.name.ifEmpty { plugin.id },
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold
                )

                // Description (if available)
                if (plugin.description.isNotEmpty()) {
                    Spacer(modifier = Modifier.height(4.dp))
                    Text(
                        text = plugin.description,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis
                    )
                }

                Spacer(modifier = Modifier.height(4.dp))
                Text(
                    text = plugin.guiTypes.joinToString(", ") { it.displayName },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
                )
            }

            // Favorite star
            IconButton(onClick = onToggleFavorite) {
                Icon(
                    imageVector = if (isFavorite) Icons.Filled.Star else Icons.Outlined.Star,
                    contentDescription = if (isFavorite) "Remove from favorites" else "Add to favorites",
                    tint = if (isFavorite) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    }
}
