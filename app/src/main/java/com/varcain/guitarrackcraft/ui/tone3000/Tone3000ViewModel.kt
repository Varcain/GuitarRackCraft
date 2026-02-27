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

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.varcain.guitarrackcraft.engine.NativeEngine
import com.varcain.guitarrackcraft.engine.RackManager
import com.varcain.guitarrackcraft.engine.X11Bridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException

class Tone3000ViewModel(application: Application) : AndroidViewModel(application) {
    private val tokenManager = TokenManager(application)
    private val api = Tone3000Api(tokenManager)

    private val _downloadStatus = MutableSharedFlow<String>(extraBufferCapacity = 1)
    val downloadStatus = _downloadStatus.asSharedFlow()

    private val _toastMessage = MutableSharedFlow<String>(extraBufferCapacity = 1)
    val toastMessage = _toastMessage.asSharedFlow()

    private val _tones = MutableStateFlow<List<Tone>>(emptyList())
    // Note: API doesn't support platform filter, so we filter locally
    private val _selectedPlatform = MutableStateFlow<Platform?>(null)
    val selectedPlatform: StateFlow<Platform?> = _selectedPlatform

    val tones: StateFlow<List<Tone>> = combine(_tones, _selectedPlatform) { tones, platform ->
        if (platform == null) tones
        else tones.filter { tone ->
            tone.platform?.lowercase() == platform.value.lowercase()
        }
    }.stateIn(viewModelScope, SharingStarted.Lazily, emptyList())

    private val _isLoading = MutableStateFlow(false)
    val isLoading: StateFlow<Boolean> = _isLoading

    private val _error = MutableStateFlow<String?>(null)
    val error: StateFlow<String?> = _error

    private val _user = MutableStateFlow<User?>(null)
    val user: StateFlow<User?> = _user

    private val _isAuthenticated = MutableStateFlow(tokenManager.hasTokens())
    val isAuthenticated: StateFlow<Boolean> = _isAuthenticated

    // Filters
    private val _selectedGear = MutableStateFlow<String?>(null)
    val selectedGear: StateFlow<String?> = _selectedGear

    private val _selectedTags = MutableStateFlow<Set<String>>(emptySet())
    val selectedTags: StateFlow<Set<String>> = _selectedTags

    private val _selectedSizes = MutableStateFlow<Set<String>>(emptySet())
    val selectedSizes: StateFlow<Set<String>> = _selectedSizes

    private val _isCalibrated = MutableStateFlow<Boolean?>(null)
    val isCalibrated: StateFlow<Boolean?> = _isCalibrated

    private val _selectedSort = MutableStateFlow(TonesSort.BEST_MATCH)
    val selectedSort: StateFlow<TonesSort> = _selectedSort

    // Model selection dialog
    private val _modelsForTone = MutableStateFlow<Pair<Tone, List<Model>>?>(null)
    val modelsForTone: StateFlow<Pair<Tone, List<Model>>?> = _modelsForTone

    // Source plugin tracking
    private var _sourcePluginIndex: Int = -1
    val sourcePluginIndex: Int get() = _sourcePluginIndex

    // Source slot (URI fragment) for multi-slot plugins like NeuralRack
    private var _sourceSlot: String? = null

    // Downloaded model IDs for reactive UI updates
    private val _downloadedModelIds = MutableStateFlow<Set<String>>(emptySet())
    val downloadedModelIds: StateFlow<Set<String>> = _downloadedModelIds

    fun setSourcePlugin(index: Int, slot: String?) {
        _sourcePluginIndex = index
        _sourceSlot = slot
    }

    private var currentPage = 1
    private var totalPages = 1
    private var currentQuery = ""

    fun initFilters(initialTag: String?, initialGear: String?, initialPlatform: String? = null) {
        _selectedGear.value = initialGear
        _selectedPlatform.value = initialPlatform?.let { platformValue ->
            Platform.values().find { it.value.lowercase() == platformValue.lowercase() }
        }
        _selectedTags.value = initialTag?.split(",")?.toSet() ?: emptySet()
        searchTones()
    }

    init {
        // searchTones() will be called by initFilters or default search
        if (_isAuthenticated.value) {
            fetchUser()
        }

        // Listen for auth callback
        viewModelScope.launch {
            Tone3000CallbackHandler.apiKeyFlow.collectLatest { apiKey ->
                exchangeApiKey(apiKey)
            }
        }

        // Listen for selection callback
        viewModelScope.launch {
            Tone3000CallbackHandler.toneUrlFlow.collectLatest { toneUrl ->
                downloadTone(toneUrl)
            }
        }
    }

    private fun handleApiError(e: Exception, prefix: String) {
        val msg = when (e) {
            is ApiException -> "$prefix: ${e.code} ${e.errorBody ?: "No details"}"
            is IOException -> "$prefix: Network error - ${e.message}"
            else -> "$prefix: ${e.message}"
        }
        _error.value = msg
        _toastMessage.tryEmit(msg)
    }

    fun setGearFilter(gear: String?) {
        _selectedGear.value = gear
        searchTones(currentQuery)
    }

    fun setPlatformFilter(platform: Platform?) {
        _selectedPlatform.value = platform
        // No searchTones here since it's local filtering
    }

    fun toggleTag(tag: String) {
        val current = _selectedTags.value.toMutableSet()
        if (current.contains(tag)) current.remove(tag) else current.add(tag)
        _selectedTags.value = current
        searchTones(currentQuery)
    }

    fun toggleSize(size: String) {
        val current = _selectedSizes.value.toMutableSet()
        if (current.contains(size)) current.remove(size) else current.add(size)
        _selectedSizes.value = current
        searchTones(currentQuery)
    }

    fun setCalibrated(calibrated: Boolean?) {
        _isCalibrated.value = calibrated
        searchTones(currentQuery)
    }

    fun setSort(sort: TonesSort) {
        _selectedSort.value = sort
        searchTones(currentQuery)
    }

    fun clearFilters() {
        _selectedGear.value = null
        _selectedPlatform.value = null
        _selectedTags.value = emptySet()
        _selectedSizes.value = emptySet()
        _isCalibrated.value = null
        _selectedSort.value = TonesSort.BEST_MATCH
        searchTones(currentQuery)
    }

    fun requestModelList(tone: Tone) {
        viewModelScope.launch {
            _isLoading.value = true
            try {
                val pageSize = tone.models_count.coerceIn(10, 100)
                val models = withContext(Dispatchers.IO) { api.getModels(tone.id, pageSize) }
                if (models.isNullOrEmpty()) {
                    _toastMessage.emit("No models found for this tone")
                } else {
                    _modelsForTone.value = tone to models
                }
            } catch (e: Exception) {
                handleApiError(e, "Fetch models failed")
            } finally {
                _isLoading.value = false
            }
        }
    }

    fun dismissModelDialog() {
        _modelsForTone.value = null
    }

    fun downloadTone(tone: Tone, model: Model) {
        dismissModelDialog()
        viewModelScope.launch {
            _isLoading.value = true
            try {
                processDownload(tone, listOf(model), model)
            } catch (e: Exception) {
                handleApiError(e, "Download failed")
            } finally {
                _isLoading.value = false
            }
        }
    }

    fun downloadTone(toneUrl: String) {
        viewModelScope.launch {
            _isLoading.value = true
            try {
                val tone = withContext(Dispatchers.IO) { api.getToneFromUrl(toneUrl) }
                if (tone == null) {
                    _error.value = "Failed to fetch tone data"
                    _isLoading.value = false
                    return@launch
                }

                val models = tone.models ?: withContext(Dispatchers.IO) { 
                    val pageSize = tone.models_count.coerceIn(10, 100)
                    api.getModels(tone.id, pageSize) 
                }
                if (models.isNullOrEmpty()) {
                    _error.value = "No compatible models found for this tone"
                    _isLoading.value = false
                    return@launch
                }

                processDownload(tone, models, useAutoFind = true)
            } catch (e: Exception) {
                handleApiError(e, "Download failed")
            } finally {
                _isLoading.value = false
            }
        }
    }

    private suspend fun processDownload(tone: Tone, models: List<Model>, preferredModel: Model? = null, useAutoFind: Boolean = false) {
        // Find best model (NAM preferred, then AIDA-X) if not specified
        val model = preferredModel ?: models.find { it.name.lowercase().contains("nam") } ?: models.firstOrNull()

        if (model == null) {
            _error.value = "No compatible models found for this tone"
            return
        }

        val fileInfo = ToneFileUtils.classifyModel(tone, model)
        val filesDir = getApplication<Application>().filesDir
        val destFile = fileInfo.resolveFile(filesDir)
        destFile.parentFile?.mkdirs()

        _downloadStatus.emit("Downloading ${tone.title}...")
        val success = withContext(Dispatchers.IO) {
            api.downloadFile(model.model_url, destFile)
        }

        if (success) {
            _downloadStatus.emit("Tone downloaded: ${destFile.name}")
            _downloadedModelIds.value = _downloadedModelIds.value + model.id

            // If it's AIDA-X, also copy to neural_models so NAM can see it
            if (fileInfo.isAidaX) {
                val neuralDir = File(filesDir, "neural_models")
                neuralDir.mkdirs()
                val targetToneDir = File(neuralDir, fileInfo.toneDirName)
                targetToneDir.mkdirs()
                destFile.copyTo(File(targetToneDir, destFile.name), overwrite = true)
            }

            // Determine which plugin to load into
            if (_sourcePluginIndex >= 0 || useAutoFind) {
                val targetPluginIndex = if (_sourcePluginIndex >= 0) {
                    // Validate source plugin still exists and is compatible
                    val pluginInfo = RackManager.getRackPluginInfo(_sourcePluginIndex)
                    if (pluginInfo != null) _sourcePluginIndex else -1
                } else {
                    // Auto-find behavior (deep-link downloads)
                    findOrAddCompatiblePlugin(fileInfo)
                }

                if (targetPluginIndex >= 0) {
                    val slot = if (_sourcePluginIndex >= 0) _sourceSlot else null
                    loadFileIntoPlugin(targetPluginIndex, fileInfo, destFile, slot)
                    _downloadStatus.emit("Tone ${tone.title} loaded into rack")
                }
            }
            // When _sourcePluginIndex == -1 and !useAutoFind: download only, no auto-load
        } else {
            _error.value = "Failed to download model file"
        }
    }

    private fun findOrAddCompatiblePlugin(fileInfo: ModelFileInfo): Int {
        val rackPlugins = RackManager.getRackPlugins()
        var targetPluginIndex = rackPlugins.indexOfFirst {
            when {
                fileInfo.isIr -> it.id.contains("ImpulseLoader") || it.id.contains("neuralrack", ignoreCase = true) || it.id.contains("aidadsp")
                fileInfo.isNam || fileInfo.isAidaX -> it.id.contains("neural-amp-modeler") || it.id.contains("neuralrack", ignoreCase = true)
                else -> it.id.contains("aidadsp") || it.id.contains("rt-neural-generic")
            }
        }

        if (targetPluginIndex == -1) {
            val available = RackManager.getAvailablePlugins()
            val pluginIdToAdd = when {
                fileInfo.isIr -> available.find { it.id.contains("ImpulseLoader") }?.id
                fileInfo.isNam -> available.find { it.id.contains("neural-amp-modeler") }?.id
                fileInfo.isAidaX -> available.find { it.id.contains("aidadsp") || it.id.contains("rt-neural-generic") }?.id
                else -> available.find { it.id.contains("aidadsp") || it.id.contains("rt-neural-generic") }?.id
            }

            if (pluginIdToAdd != null) {
                targetPluginIndex = RackManager.addPlugin(pluginIdToAdd)
            }
        }
        return targetPluginIndex
    }

    private fun loadFileIntoPlugin(pluginIndex: Int, fileInfo: ModelFileInfo, file: File, slot: String? = null) {
        val rackPlugin = RackManager.getRackPluginInfo(pluginIndex)
        val propertyUri = ToneFileUtils.resolvePropertyUri(fileInfo, rackPlugin?.id, slot)
        NativeEngine.getInstance().setPluginFilePath(pluginIndex, propertyUri, file.absolutePath)
        X11Bridge.deliverFileToPluginUI(pluginIndex, propertyUri, file.absolutePath)
        RackManager.notifyModelLoaded(pluginIndex, file.nameWithoutExtension)
    }

    fun loadModelToPlugin(tone: Tone, model: Model) {
        viewModelScope.launch {
            val fileInfo = ToneFileUtils.classifyModel(tone, model)
            val filesDir = getApplication<Application>().filesDir
            val destFile = fileInfo.resolveFile(filesDir)

            if (!destFile.exists()) {
                _toastMessage.tryEmit("Model file not found on disk")
                return@launch
            }

            if (_sourcePluginIndex < 0) {
                _toastMessage.tryEmit("Already downloaded")
                return@launch
            }

            val pluginInfo = RackManager.getRackPluginInfo(_sourcePluginIndex)
            if (pluginInfo == null) {
                _toastMessage.tryEmit("Source plugin was removed")
                return@launch
            }

            loadFileIntoPlugin(_sourcePluginIndex, fileInfo, destFile, _sourceSlot)
            _downloadStatus.tryEmit("Model loaded into rack")
        }
    }

    private fun fetchUser() {
        viewModelScope.launch {
            try {
                val userResult = withContext(Dispatchers.IO) { api.getUser() }
                _user.value = userResult
            } catch (e: Exception) {
                // If fetching user fails, maybe tokens are invalid
                if (e is ApiException && e.code == 401) {
                    logout()
                }
                handleApiError(e, "Fetch user failed")
            }
        }
    }

    private fun exchangeApiKey(apiKey: String) {
        viewModelScope.launch {
            _isLoading.value = true
            try {
                val session = withContext(Dispatchers.IO) {
                    api.exchangeApiKey(apiKey)
                }
                if (session != null) {
                    tokenManager.accessToken = session.access_token
                    tokenManager.refreshToken = session.refresh_token
                    _isAuthenticated.value = true
                    _toastMessage.emit("Logged in successfully")
                    fetchUser()
                    searchTones() // Refresh search with auth
                } else {
                    _error.value = "Authentication failed (no session)"
                }
            } catch (e: Exception) {
                handleApiError(e, "Authentication failed")
            } finally {
                _isLoading.value = false
            }
        }
    }

    fun logout() {
        tokenManager.clear()
        _isAuthenticated.value = false
        _user.value = null
        _toastMessage.tryEmit("Logged out")
        searchTones()
    }

    fun searchTones(query: String = "") {
        currentQuery = query
        viewModelScope.launch {
            _isLoading.value = true
            _error.value = null
            currentPage = 1
            
            val effectiveQuery = if (_selectedTags.value.isEmpty()) {
                query
            } else {
                val tagsString = _selectedTags.value.joinToString(" ")
                if (query.isEmpty()) tagsString else "$query $tagsString"
            }

            try {
                val result = withContext(Dispatchers.IO) {
                    api.searchTones(
                        query = effectiveQuery,
                        page = currentPage,
                        pageSize = 25,
                        gear = _selectedGear.value,
                        sizes = if (_selectedSizes.value.isEmpty()) null else _selectedSizes.value.joinToString(","),
                        calibrated = _isCalibrated.value,
                        sort = _selectedSort.value.value
                    )
                }

                if (result != null) {
                    _tones.value = result.data
                    totalPages = result.total_pages
                    
                    // If we have a platform filter and didn't get enough results, load more
                    val platform = _selectedPlatform.value
                    if (platform != null) {
                        var currentFilteredCount = result.data.count { it.platform?.lowercase() == platform.value.lowercase() }
                        while (currentFilteredCount < 10 && currentPage < totalPages) {
                            currentPage++
                            val nextPageResult = withContext(Dispatchers.IO) {
                                api.searchTones(
                                    query = effectiveQuery,
                                    page = currentPage,
                                    pageSize = 25,
                                    gear = _selectedGear.value,
                                    sizes = if (_selectedSizes.value.isEmpty()) null else _selectedSizes.value.joinToString(","),
                                    calibrated = _isCalibrated.value,
                                    sort = _selectedSort.value.value
                                )
                            }
                            if (nextPageResult != null) {
                                _tones.value = _tones.value + nextPageResult.data
                                currentFilteredCount += nextPageResult.data.count { it.platform?.lowercase() == platform.value.lowercase() }
                            } else {
                                break
                            }
                        }
                    }
                } else {
                    _error.value = "Failed to load tones (no data)"
                }
            } catch (e: Exception) {
                handleApiError(e, "Failed to load tones")
            } finally {
                _isLoading.value = false
            }
        }
    }

    fun loadNextPage() {
        if (_isLoading.value || currentPage >= totalPages) return

        viewModelScope.launch {
            _isLoading.value = true
            
            val effectiveQuery = if (_selectedTags.value.isEmpty()) {
                currentQuery
            } else {
                val tagsString = _selectedTags.value.joinToString(" ")
                if (currentQuery.isEmpty()) tagsString else "$currentQuery $tagsString"
            }

            try {
                var moreNeeded = true
                val platform = _selectedPlatform.value
                
                while (moreNeeded && currentPage < totalPages) {
                    currentPage++
                    val result = withContext(Dispatchers.IO) {
                        api.searchTones(
                            query = effectiveQuery,
                            page = currentPage,
                            pageSize = 25,
                            gear = _selectedGear.value,
                            sizes = if (_selectedSizes.value.isEmpty()) null else _selectedSizes.value.joinToString(","),
                            calibrated = _isCalibrated.value,
                            sort = _selectedSort.value.value
                        )
                    }

                    if (result != null) {
                        _tones.value = _tones.value + result.data
                        
                        // If no platform filter, one page is enough.
                        // If platform filter active, continue until we get at least some matches
                        if (platform == null) {
                            moreNeeded = false
                        } else {
                            val newMatches = result.data.count { it.platform?.lowercase() == platform.value.lowercase() }
                            if (newMatches > 0) {
                                moreNeeded = false
                            }
                        }
                    } else {
                        moreNeeded = false
                    }
                }
            } catch (e: Exception) {
                handleApiError(e, "Failed to load more tones")
            } finally {
                _isLoading.value = false
            }
        }
    }
}
