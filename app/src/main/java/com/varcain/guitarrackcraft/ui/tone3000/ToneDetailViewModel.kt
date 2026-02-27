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

class ToneDetailViewModel(application: Application) : AndroidViewModel(application) {
    private val tokenManager = TokenManager(application)
    private val api = Tone3000Api(tokenManager)

    private val _tone = MutableStateFlow<Tone?>(null)
    val tone: StateFlow<Tone?> = _tone

    private val _models = MutableStateFlow<List<Model>>(emptyList())
    val models: StateFlow<List<Model>> = _models

    private val _isLoading = MutableStateFlow(false)
    val isLoading: StateFlow<Boolean> = _isLoading

    private val _error = MutableStateFlow<String?>(null)
    val error: StateFlow<String?> = _error

    private val _modelsError = MutableStateFlow<String?>(null)
    val modelsError: StateFlow<String?> = _modelsError

    private val _downloadStatus = MutableSharedFlow<String>(extraBufferCapacity = 1)
    val downloadStatus = _downloadStatus.asSharedFlow()

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

    fun setTone(tone: Tone) {
        _tone.value = tone
    }

    fun loadToneDetail(toneId: String) {
        viewModelScope.launch {
            _isLoading.value = true
            _modelsError.value = null
            
            // 1. Try fetching tone detail (might fail with 404, but that's okay if we have initialTone)
            if (_tone.value == null) {
                try {
                    val toneResult = withContext(Dispatchers.IO) { 
                        api.getToneFromUrl("/tones/$toneId") 
                    }
                    _tone.value = toneResult
                } catch (e: Exception) {
                    android.util.Log.w("ToneDetail", "Failed to fetch tone detail: ${e.message}")
                    if (_tone.value == null) {
                        _error.value = "Failed to load tone info"
                    }
                }
            }

            // 2. Fetch models (required for download)
            try {
                val pageSize = _tone.value?.models_count?.coerceIn(10, 100) ?: 10
                val modelsResult = withContext(Dispatchers.IO) {
                    api.getModels(toneId, pageSize)
                }
                _models.value = modelsResult ?: emptyList()
                if (modelsResult.isNullOrEmpty()) {
                    _modelsError.value = "No models available for this tone"
                }
            } catch (e: Exception) {
                android.util.Log.e("ToneDetail", "Failed to fetch models: ${e.message}")
                if (e is ApiException && e.code == 401) {
                    _modelsError.value = "Please login to view and download models"
                } else {
                    _modelsError.value = "Failed to load models list"
                }
            } finally {
                _isLoading.value = false
            }
        }
    }

    fun downloadModel(tone: Tone, model: Model) {
        viewModelScope.launch {
            _isLoading.value = true
            try {
                val fileInfo = ToneFileUtils.classifyModel(tone, model)
                val filesDir = getApplication<Application>().filesDir
                val destFile = fileInfo.resolveFile(filesDir)
                destFile.parentFile?.mkdirs()

                _downloadStatus.emit("Downloading model ${model.name}...")
                val success = withContext(Dispatchers.IO) {
                    api.downloadFile(model.model_url, destFile)
                }

                if (success) {
                    _downloadStatus.emit("Model downloaded: ${destFile.name}")
                    _downloadedModelIds.value = _downloadedModelIds.value + model.id

                    // If it's AIDA-X, also copy to neural_models so NAM can see it
                    if (fileInfo.isAidaX) {
                        val neuralDir = File(filesDir, "neural_models")
                        neuralDir.mkdirs()
                        val targetToneDir = File(neuralDir, fileInfo.toneDirName)
                        targetToneDir.mkdirs()
                        destFile.copyTo(File(targetToneDir, destFile.name), overwrite = true)
                    }

                    // Load into plugin based on source context
                    if (_sourcePluginIndex >= 0) {
                        val pluginInfo = RackManager.getRackPluginInfo(_sourcePluginIndex)
                        if (pluginInfo != null) {
                            loadFileIntoPlugin(_sourcePluginIndex, fileInfo, destFile, _sourceSlot)
                            _downloadStatus.emit("Model loaded into rack")
                        }
                    }
                    // When _sourcePluginIndex == -1: download only, no auto-load
                } else {
                    _error.value = "Failed to download model file"
                }
            } catch (e: Exception) {
                _error.value = "Download failed: ${e.message}"
            } finally {
                _isLoading.value = false
            }
        }
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
                _downloadStatus.tryEmit("Model file not found on disk")
                return@launch
            }

            if (_sourcePluginIndex < 0) {
                _downloadStatus.tryEmit("Already downloaded")
                return@launch
            }

            val pluginInfo = RackManager.getRackPluginInfo(_sourcePluginIndex)
            if (pluginInfo == null) {
                _downloadStatus.tryEmit("Source plugin was removed")
                return@launch
            }

            loadFileIntoPlugin(_sourcePluginIndex, fileInfo, destFile, _sourceSlot)
            _downloadStatus.tryEmit("Model loaded into rack")
        }
    }
}
