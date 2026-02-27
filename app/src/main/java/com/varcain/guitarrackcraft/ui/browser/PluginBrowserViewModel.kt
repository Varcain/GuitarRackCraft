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

import android.app.Application
import android.content.Context
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.varcain.guitarrackcraft.engine.FavoritesManager
import com.varcain.guitarrackcraft.engine.PluginInfo
import com.varcain.guitarrackcraft.engine.RackManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject

/**
 * Category information for a plugin.
 */
data class PluginCategory(
    val name: String,
    val plugins: List<PluginInfo>
)

/**
 * Author group containing categories of plugins.
 */
data class AuthorGroup(
    val name: String,
    val categories: List<PluginCategory>
)

/**
 * Mapping of plugin names to their categories based on GxPlugins.lv2 README.
 * Plugins are matched by checking if the plugin name contains the key (case-insensitive).
 */
/**
 * Plugin metadata loaded from assets.
 */
data class PluginMetadata(
    val descriptions: Map<String, String>,
    val thumbnails: Map<String, String>,
    val authors: Map<String, String>,
    val categories: Map<String, String>
)

object PluginCategoryMapping {

    const val AUTHOR_GXPLUGINS = "GxPlugins"
    const val AUTHOR_GUITARIX = "Guitarix"
    const val AUTHOR_NEURAL_AMP = "Neural Amp Modeler"
    const val AUTHOR_AIDA_DSP = "Aida DSP"
    const val AUTHOR_BRUMMER10 = "brummer10"
    const val AUTHOR_UNKNOWN = "Unknown"

    /** Maps LV2 plugin class names to display categories. */
    private val LV2_CLASS_TO_CATEGORY = mapOf(
        "DistortionPlugin" to "Distortion",
        "AmplifierPlugin" to "Amplifier",
        "SimulatorPlugin" to "Simulator",
        "DelayPlugin" to "Delay",
        "ModulatorPlugin" to "Modulator",
        "FilterPlugin" to "Filter",
        "ReverbPlugin" to "Reverb",
        "EQPlugin" to "EQ",
        "CompressorPlugin" to "Compressor",
        "PitchPlugin" to "Pitch",
        "ChorusPlugin" to "Modulator",
        "FlangerPlugin" to "Modulator",
        "PhaserPlugin" to "Modulator",
        "EnvelopePlugin" to "Dynamics",
        "GatePlugin" to "Dynamics",
        "ExpanderPlugin" to "Dynamics",
        "UtilityPlugin" to "Utility",
        "AnalyserPlugin" to "Utility"
    )

    /**
     * Determines the category for a plugin using metadata LV2 class.
     * Falls back to "Other" if no class is found.
     */
    fun getCategory(pluginName: String, metadataCategories: Map<String, String> = emptyMap()): String {
        val normalizedName = pluginName.lowercase()
        val lv2Class = metadataCategories.entries
            .find { it.key.lowercase() == normalizedName }
            ?.value
        if (lv2Class != null) {
            return LV2_CLASS_TO_CATEGORY[lv2Class] ?: "Other"
        }
        return "Other"
    }
    
    /**
     * Determines the author for a plugin.
     * Currently identifies GxPlugins based on name prefix.
     */
    fun getAuthor(pluginName: String, metadataAuthors: Map<String, String> = emptyMap()): String {
        // Check metadata authors map first (case-insensitive)
        val normalizedName = pluginName.lowercase()
        metadataAuthors.entries.find { it.key.lowercase() == normalizedName }?.let {
            return it.value
        }
        return if (pluginName.startsWith("Gx", ignoreCase = true)) {
            AUTHOR_GXPLUGINS
        } else {
            AUTHOR_UNKNOWN
        }
    }
}

class PluginBrowserViewModel(application: Application) : AndroidViewModel(application) {

    companion object {
        const val FAVORITES_GROUP = "Favorites"
    }

    private val appContext: Context = application.applicationContext
    
    private val _plugins = MutableStateFlow<List<PluginInfo>>(emptyList())
    val plugins: StateFlow<List<PluginInfo>> = _plugins.asStateFlow()
    
    private val _groupedPlugins = MutableStateFlow<List<AuthorGroup>>(emptyList())
    val groupedPlugins: StateFlow<List<AuthorGroup>> = _groupedPlugins.asStateFlow()
    
    private val _isLoading = MutableStateFlow(false)
    val isLoading: StateFlow<Boolean> = _isLoading.asStateFlow()
    
    private val _errorMessage = MutableStateFlow<String?>(null)
    val errorMessage: StateFlow<String?> = _errorMessage.asStateFlow()

    /** Shown as Snackbar when add-to-rack fails (e.g. plugin binary missing). */
    private val _addFailureMessage = MutableStateFlow<String?>(null)
    val addFailureMessage: StateFlow<String?> = _addFailureMessage.asStateFlow()
    
    private val _favorites = MutableStateFlow<Set<String>>(emptySet())
    val favorites: StateFlow<Set<String>> = _favorites.asStateFlow()

    /** Tracks which authors are expanded. Initially only Favorites is expanded. */
    private val _expandedAuthors = MutableStateFlow<Set<String>>(setOf(FAVORITES_GROUP))
    val expandedAuthors: StateFlow<Set<String>> = _expandedAuthors.asStateFlow()
    
    /** Tracks which categories are expanded (key is "Author|Category") */
    private val _expandedCategories = MutableStateFlow<Set<String>>(emptySet())
    val expandedCategories: StateFlow<Set<String>> = _expandedCategories.asStateFlow()
    
    /** Plugin metadata loaded from assets */
    private var pluginMetadata: PluginMetadata? = null
    
    /** Set of plugin names that have working binaries */
    private var availablePlugins: Set<String> = emptySet()

    init {
        _favorites.value = FavoritesManager.getFavorites(appContext)
        viewModelScope.launch {
            withContext(Dispatchers.IO) { loadMetadata() }
            loadPluginsInternal()
        }
    }
    
    /**
     * Load plugin metadata (descriptions and thumbnails) from JSON file.
     */
    private fun loadMetadata() {
        try {
            appContext.assets.open("plugin_metadata.json").use { inputStream ->
                val jsonString = inputStream.bufferedReader().use { it.readText() }
                val json = JSONObject(jsonString)
                
                val descriptions = mutableMapOf<String, String>()
                val thumbnails = mutableMapOf<String, String>()
                val available = mutableSetOf<String>()
                
                json.optJSONObject("descriptions")?.let { descObj ->
                    descObj.keys().forEach { key ->
                        descriptions[key] = descObj.optString(key, "")
                    }
                }
                
                json.optJSONObject("thumbnails")?.let { thumbObj ->
                    thumbObj.keys().forEach { key ->
                        thumbnails[key] = thumbObj.optString(key, "")
                    }
                }
                
                json.optJSONArray("availablePlugins")?.let { availableArr ->
                    for (i in 0 until availableArr.length()) {
                        available.add(availableArr.optString(i, ""))
                    }
                }
                
                val authors = mutableMapOf<String, String>()
                json.optJSONObject("authors")?.let { authObj ->
                    authObj.keys().forEach { key ->
                        authors[key] = authObj.optString(key, "")
                    }
                }

                val categories = mutableMapOf<String, String>()
                json.optJSONObject("categories")?.let { catObj ->
                    catObj.keys().forEach { key ->
                        categories[key] = catObj.optString(key, "")
                    }
                }

                pluginMetadata = PluginMetadata(descriptions, thumbnails, authors, categories)
                availablePlugins = available
                
                android.util.Log.i("PluginBrowser", "Loaded ${available.size} available plugins with binaries")
            }
        } catch (e: Exception) {
            android.util.Log.w("PluginBrowser", "Failed to load plugin metadata: ${e.message}")
            pluginMetadata = PluginMetadata(emptyMap(), emptyMap(), emptyMap(), emptyMap())
            availablePlugins = emptySet()
        }
    }
    
    /**
     * Apply metadata (description and thumbnail) to a plugin.
     * Uses case-insensitive matching since plugin names can have different casing
     * (e.g., "GxVMK2" vs "GxVmk2").
     */
    private fun applyMetadata(plugin: PluginInfo): PluginInfo {
        val metadata = pluginMetadata ?: return plugin
        
        // Find matching metadata by plugin name (case-insensitive)
        val normalizedPluginName = plugin.name.lowercase()
        
        val description = metadata.descriptions.entries
            .find { it.key.lowercase() == normalizedPluginName }
            ?.value ?: ""
        
        val thumbnailPath = metadata.thumbnails.entries
            .find { it.key.lowercase() == normalizedPluginName }
            ?.value ?: ""
        
        return plugin.copy(
            description = description,
            thumbnailPath = thumbnailPath
        )
    }

    private fun loadPlugins() {
        viewModelScope.launch { loadPluginsInternal() }
    }

    private suspend fun loadPluginsInternal() {
        _isLoading.value = true
        _errorMessage.value = null

        try {
            val allPlugins = RackManager.getAvailablePlugins()
            val availablePluginNamesLower = availablePlugins.map { it.lowercase() }.toSet()

            // If metadata lists specific available plugins, filter to only those.
            // If the list is empty (no metadata), show all plugins from the native engine.
            val filteredPlugins = if (availablePluginNamesLower.isEmpty()) {
                allPlugins.map { applyMetadata(it) }
            } else {
                allPlugins.filter { plugin ->
                    availablePluginNamesLower.contains(plugin.name.lowercase())
                }.map { applyMetadata(it) }
            }

            android.util.Log.i("PluginBrowser", "Showing ${filteredPlugins.size} out of ${allPlugins.size} discovered plugins (metadata filter=${availablePluginNamesLower.size})")

            _plugins.value = filteredPlugins
            _groupedPlugins.value = groupPluginsByAuthorAndCategory(filteredPlugins)
        } catch (e: Exception) {
            _errorMessage.value = "Failed to load plugins: ${e.message}"
        } finally {
            _isLoading.value = false
        }
    }
    
    /**
     * Groups plugins first by author, then by category within each author.
     */
    private fun groupPluginsByAuthorAndCategory(plugins: List<PluginInfo>): List<AuthorGroup> {
        val metadataAuthors = pluginMetadata?.authors ?: emptyMap()
        val metadataCategories = pluginMetadata?.categories ?: emptyMap()
        val byAuthor = plugins.groupBy { PluginCategoryMapping.getAuthor(it.name, metadataAuthors) }

        val authorGroups = byAuthor.map { (author, authorPlugins) ->
            val byCategory = authorPlugins.groupBy { PluginCategoryMapping.getCategory(it.name, metadataCategories) }

            val sortedCategories = byCategory.toList()
                .sortedBy { (category, _) ->
                    if (category == "Other") "\uFFFF" else category
                }
                .map { (category, catPlugins) ->
                    PluginCategory(
                        name = category,
                        plugins = catPlugins.sortedBy { it.name }
                    )
                }

            AuthorGroup(
                name = author,
                categories = sortedCategories
            )
        }.sortedBy {
            when (it.name) {
                PluginCategoryMapping.AUTHOR_NEURAL_AMP -> 0
                PluginCategoryMapping.AUTHOR_AIDA_DSP -> 1
                PluginCategoryMapping.AUTHOR_GUITARIX -> 2
                PluginCategoryMapping.AUTHOR_BRUMMER10 -> 3
                PluginCategoryMapping.AUTHOR_GXPLUGINS -> 4
                else -> 5
            }
        }

        // Prepend Favorites group if any favorites exist
        val favIds = _favorites.value
        if (favIds.isEmpty()) return authorGroups

        val favPlugins = plugins.filter { favIds.contains(it.fullId) }
        if (favPlugins.isEmpty()) return authorGroups

        val byCategory = favPlugins.groupBy { PluginCategoryMapping.getCategory(it.name, metadataCategories) }
        val favCategories = byCategory.toList()
            .sortedBy { (category, _) ->
                if (category == "Other") "\uFFFF" else category
            }
            .map { (category, catPlugins) ->
                PluginCategory(
                    name = category,
                    plugins = catPlugins.sortedBy { it.name }
                )
            }
        val favGroup = AuthorGroup(name = FAVORITES_GROUP, categories = favCategories)
        return listOf(favGroup) + authorGroups
    }
    
    /**
     * Toggle author expansion state.
     */
    fun toggleAuthor(authorName: String) {
        _expandedAuthors.value = _expandedAuthors.value.toMutableSet().apply {
            if (contains(authorName)) {
                remove(authorName)
            } else {
                add(authorName)
            }
        }
    }
    
    /**
     * Toggle category expansion state.
     */
    fun toggleCategory(authorName: String, categoryName: String) {
        val key = "$authorName|$categoryName"
        _expandedCategories.value = _expandedCategories.value.toMutableSet().apply {
            if (contains(key)) {
                remove(key)
            } else {
                add(key)
            }
        }
    }

    fun addPluginToRack(plugin: PluginInfo, position: Int = -1): Boolean {
        android.util.Log.i("PluginBrowser", "[LIFECYCLE] addPluginToRack called: ${plugin.name} (${plugin.fullId})")
        return try {
            val index = RackManager.addPlugin(plugin.fullId, position)
            android.util.Log.i("PluginBrowser", "[LIFECYCLE] addPluginToRack result: ${plugin.name} -> index=$index")
            if (index >= 0) {
                true
            } else {
                _addFailureMessage.value = "Could not add plugin. Plugin binaries (.so) are not included in this build—only metadata is available."
                false
            }
        } catch (e: Exception) {
            android.util.Log.e("PluginBrowser", "[LIFECYCLE] addPluginToRack failed: ${plugin.name}", e)
            _addFailureMessage.value = "Failed to add plugin: ${e.message}"
            false
        }
    }

    fun replacePluginInRack(position: Int, plugin: PluginInfo): Boolean {
        android.util.Log.i("PluginBrowser", "[LIFECYCLE] replacePluginInRack called: position=$position, ${plugin.name} (${plugin.fullId})")
        return try {
            RackManager.removePlugin(position)
            val index = RackManager.addPlugin(plugin.fullId, position)
            android.util.Log.i("PluginBrowser", "[LIFECYCLE] replacePluginInRack result: ${plugin.name} -> index=$index")
            if (index >= 0) {
                true
            } else {
                _addFailureMessage.value = "Could not add plugin. Plugin binaries (.so) are not included in this build—only metadata is available."
                false
            }
        } catch (e: Exception) {
            android.util.Log.e("PluginBrowser", "[LIFECYCLE] replacePluginInRack failed: ${plugin.name}", e)
            _addFailureMessage.value = "Failed to replace plugin: ${e.message}"
            false
        }
    }

    fun clearAddFailureMessage() {
        _addFailureMessage.value = null
    }

    fun toggleFavorite(pluginId: String) {
        FavoritesManager.toggleFavorite(appContext, pluginId)
        _favorites.value = FavoritesManager.getFavorites(appContext)
        // Regroup to add/remove from Favorites section
        _groupedPlugins.value = groupPluginsByAuthorAndCategory(_plugins.value)
    }

    fun refresh() {
        loadPlugins()
    }
}
