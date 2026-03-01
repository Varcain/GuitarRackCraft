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

import android.app.Application
import android.content.Context
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.varcain.guitarrackcraft.engine.AudioEngine
import com.varcain.guitarrackcraft.engine.NativeEngine
import com.varcain.guitarrackcraft.engine.PresetManager
import com.varcain.guitarrackcraft.engine.RackManager
import com.varcain.guitarrackcraft.engine.PluginInfo
import com.varcain.guitarrackcraft.engine.PluginUiPreferenceManager
import com.varcain.guitarrackcraft.engine.RecordingManager
import com.varcain.guitarrackcraft.engine.RecentPresetsManager
import com.varcain.guitarrackcraft.engine.UiType
import com.varcain.guitarrackcraft.engine.WavPlayer
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class RackPlugin(
    val index: Int,
    val name: String,
    val pluginId: String,
    val instanceId: Long = nextInstanceId()
) {
    companion object {
        private val counter = java.util.concurrent.atomic.AtomicLong(0)
        fun nextInstanceId(): Long = counter.getAndIncrement()
    }
}

class RackViewModel(application: Application) : AndroidViewModel(application) {

    private val _isEngineRunning = MutableStateFlow(false)
    val isEngineRunning: StateFlow<Boolean> = _isEngineRunning.asStateFlow()

    private val _latencyMs = MutableStateFlow(0.0)
    val latencyMs: StateFlow<Double> = _latencyMs.asStateFlow()

    private val _inputLevel = MutableStateFlow(0f)
    val inputLevel: StateFlow<Float> = _inputLevel.asStateFlow()

    private val _outputLevel = MutableStateFlow(0f)
    val outputLevel: StateFlow<Float> = _outputLevel.asStateFlow()

    private val _cpuLoad = MutableStateFlow(0f)
    val cpuLoad: StateFlow<Float> = _cpuLoad.asStateFlow()

    private val _xRunCount = MutableStateFlow(0)
    val xRunCount: StateFlow<Int> = _xRunCount.asStateFlow()

    private val _inputClipping = MutableStateFlow(false)
    val inputClipping: StateFlow<Boolean> = _inputClipping.asStateFlow()

    private val _outputClipping = MutableStateFlow(false)
    val outputClipping: StateFlow<Boolean> = _outputClipping.asStateFlow()

    private val _rackPlugins = MutableStateFlow<List<RackPlugin>>(emptyList())
    val rackPlugins: StateFlow<List<RackPlugin>> = _rackPlugins.asStateFlow()

    private val _errorMessage = MutableStateFlow<String?>(null)
    val errorMessage: StateFlow<String?> = _errorMessage.asStateFlow()

    // Preset state
    private val presetManager = PresetManager(NativeEngine.getInstance())
    private var recentPresetsManager: RecentPresetsManager? = null

    private val _presetList = MutableStateFlow<List<String>>(emptyList())
    val presetList: StateFlow<List<String>> = _presetList.asStateFlow()

    private val _recentPresets = MutableStateFlow<List<String>>(emptyList())
    val recentPresets: StateFlow<List<String>> = _recentPresets.asStateFlow()

    private val _presetMessage = MutableStateFlow<String?>(null)
    val presetMessage: StateFlow<String?> = _presetMessage.asStateFlow()

    // WAV playback state
    private val _wavLoaded = MutableStateFlow(false)
    val wavLoaded: StateFlow<Boolean> = _wavLoaded.asStateFlow()

    private val _wavDurationSec = MutableStateFlow(0.0)
    val wavDurationSec: StateFlow<Double> = _wavDurationSec.asStateFlow()

    private val _wavPositionSec = MutableStateFlow(0.0)
    val wavPositionSec: StateFlow<Double> = _wavPositionSec.asStateFlow()

    private val _isWavPlaying = MutableStateFlow(false)
    val isWavPlaying: StateFlow<Boolean> = _isWavPlaying.asStateFlow()

    private val _loadedFileName = MutableStateFlow<String?>(null)
    val loadedFileName: StateFlow<String?> = _loadedFileName.asStateFlow()

    private val _wavRepeat = MutableStateFlow(false)
    val wavRepeat: StateFlow<Boolean> = _wavRepeat.asStateFlow()

    private val _wavProcessEffects = MutableStateFlow(false)
    val wavProcessEffects: StateFlow<Boolean> = _wavProcessEffects.asStateFlow()

    // Recording state
    private val _isRecording = MutableStateFlow(false)
    val isRecording: StateFlow<Boolean> = _isRecording.asStateFlow()

    private val _recordingDurationSec = MutableStateFlow(0.0)
    val recordingDurationSec: StateFlow<Double> = _recordingDurationSec.asStateFlow()

    init {
        val ctx = getApplication<Application>()
        val inputId = com.varcain.guitarrackcraft.engine.AudioSettingsManager.getInputDeviceId(ctx)
        val outputId = com.varcain.guitarrackcraft.engine.AudioSettingsManager.getOutputDeviceId(ctx)
        val bufSize = com.varcain.guitarrackcraft.engine.AudioSettingsManager.getBufferSize(ctx)
        startEngine(inputDeviceId = inputId, outputDeviceId = outputId, bufferFrames = bufSize)
        updateRackState()

        // Refresh rack state periodically to catch external changes
        viewModelScope.launch {
            delay(1000)
            updateRackState()
        }

        // Poll meters when engine is running
        viewModelScope.launch {
            var cpuSum = 0f
            var latencySum = 0.0
            var sampleCount = 0
            while (true) {
                if (_isEngineRunning.value) {
                    try {
                        _inputLevel.value = AudioEngine.getInputLevel()
                        _outputLevel.value = AudioEngine.getOutputLevel()
                        _inputClipping.value = AudioEngine.isInputClipping()
                        _outputClipping.value = AudioEngine.isOutputClipping()
                        cpuSum += AudioEngine.getCpuLoad()
                        latencySum += AudioEngine.getLatencyMs()
                        sampleCount++
                        if (sampleCount >= 20) { // ~1 second (20 × 50ms)
                            _cpuLoad.value = cpuSum / sampleCount
                            _latencyMs.value = latencySum / sampleCount
                            _xRunCount.value = AudioEngine.getXRunCount()
                            cpuSum = 0f
                            latencySum = 0.0
                            sampleCount = 0
                        }
                        // Poll recording duration
                        if (_isRecording.value) {
                            _recordingDurationSec.value = RecordingManager.getRecordingDurationSec()
                        }
                    } catch (_: Exception) { }
                } else {
                    cpuSum = 0f
                    latencySum = 0.0
                    sampleCount = 0
                }
                delay(50)
            }
        }
    }

    fun startEngine(inputDeviceId: Int = 0, outputDeviceId: Int = 0, bufferFrames: Int = 0) {
        android.util.Log.i("AudioLifecycle", "RackViewModel.startEngine(input=$inputDeviceId, output=$outputDeviceId, buf=$bufferFrames) (thread=${Thread.currentThread().name})")
        viewModelScope.launch {
            try {
                val started = AudioEngine.start(
                    inputDeviceId = inputDeviceId,
                    outputDeviceId = outputDeviceId,
                    bufferFrames = bufferFrames
                )
                android.util.Log.i("AudioLifecycle", "RackViewModel.startEngine() result=$started")
                _isEngineRunning.value = started
                if (started) {
                    _errorMessage.value = null
                }
            } catch (e: Exception) {
                _errorMessage.value = "Failed to start engine: ${e.message}"
            }
        }
    }

    private var restartJob: Job? = null

    fun restartEngine(context: Context) {
        restartJob?.cancel()
        restartJob = viewModelScope.launch {
            val inputId = com.varcain.guitarrackcraft.engine.AudioSettingsManager.getInputDeviceId(context)
            val outputId = com.varcain.guitarrackcraft.engine.AudioSettingsManager.getOutputDeviceId(context)
            val bufSize = com.varcain.guitarrackcraft.engine.AudioSettingsManager.getBufferSize(context)
            android.util.Log.i("AudioLifecycle", "RackViewModel.restartEngine(input=$inputId, output=$outputId, buf=$bufSize)")
            stopEngine()
            delay(100)
            startEngine(inputDeviceId = inputId, outputDeviceId = outputId, bufferFrames = bufSize)
        }
    }

    fun resetClipping() {
        AudioEngine.resetClipping()
        _inputClipping.value = false
        _outputClipping.value = false
    }

    fun stopEngine() {
        android.util.Log.i("AudioLifecycle", "RackViewModel.stopEngine() -> native (thread=${Thread.currentThread().name})")
        stopRecording()
        AudioEngine.stop()
        _isEngineRunning.value = false
    }

    fun toggleRecording(context: Context) {
        if (_isRecording.value) {
            stopRecording()
        } else {
            if (!_isEngineRunning.value) {
                _errorMessage.value = "Start the engine first to record"
                return
            }
            val started = RecordingManager.startRecording(context)
            _isRecording.value = started
            if (!started) {
                _errorMessage.value = "Failed to start recording"
            }
        }
    }

    fun stopRecording() {
        if (_isRecording.value) {
            RecordingManager.stopRecording()
            _isRecording.value = false
            _recordingDurationSec.value = 0.0
        }
    }

    fun removePlugin(position: Int) {
        viewModelScope.launch {
            try {
                if (RackManager.removePlugin(position)) {
                    updateRackState()
                } else {
                    _errorMessage.value = "Failed to remove plugin at position $position"
                }
            } catch (e: Exception) {
                _errorMessage.value = "Failed to remove plugin: ${e.message}"
            }
        }
    }

    fun reorderPlugins(fromPos: Int, toPos: Int) {
        viewModelScope.launch {
            try {
                if (RackManager.reorder(fromPos, toPos)) {
                    updateRackState()
                } else {
                    _errorMessage.value = "Failed to reorder plugins"
                }
            } catch (e: Exception) {
                _errorMessage.value = "Failed to reorder plugins: ${e.message}"
            }
        }
    }

    fun refreshRack() {
        updateRackState()
    }

    fun loadWav(path: String, fileName: String? = null) {
        viewModelScope.launch {
            try {
                if (!AudioEngine.isRunning()) {
                    _errorMessage.value = "Start the engine first to load a WAV file"
                    return@launch
                }
                val success = withContext(Dispatchers.IO) {
                    WavPlayer.load(path)
                }
                if (success) {
                    _wavLoaded.value = true
                    _wavDurationSec.value = WavPlayer.getDurationSec()
                    _wavPositionSec.value = 0.0
                    _isWavPlaying.value = false
                    _loadedFileName.value = fileName ?: path.substringAfterLast('/')
                    _errorMessage.value = null
                } else {
                    _errorMessage.value = "Failed to load WAV file"
                }
            } catch (e: Exception) {
                _errorMessage.value = "Failed to load WAV: ${e.message}"
            }
        }
    }

    fun wavPlay() {
        try {
            WavPlayer.play()
            _isWavPlaying.value = WavPlayer.isPlaying()
        } catch (_: Exception) { }
    }

    fun wavPause() {
        try {
            WavPlayer.pause()
            _isWavPlaying.value = false
        } catch (_: Exception) { }
    }

    fun wavSeek(positionSec: Double) {
        try {
            WavPlayer.seek(positionSec)
            _wavPositionSec.value = positionSec
        } catch (_: Exception) { }
    }

    fun updateWavPosition() {
        try {
            _wavPositionSec.value = WavPlayer.getPositionSec()
            _isWavPlaying.value = WavPlayer.isPlaying()
        } catch (_: Exception) { }
    }

    fun unloadWav() {
        try {
            WavPlayer.unload()
            _wavLoaded.value = false
            _wavDurationSec.value = 0.0
            _wavPositionSec.value = 0.0
            _isWavPlaying.value = false
            _loadedFileName.value = null
        } catch (_: Exception) { }
    }

    fun wavRestart() {
        wavSeek(0.0)
        wavPlay()
    }

    fun wavToggleRepeat() {
        _wavRepeat.value = !_wavRepeat.value
    }

    fun wavToggleProcessEffects() {
        setWavProcessEffects(!_wavProcessEffects.value)
    }

    fun setWavProcessEffects(enabled: Boolean) {
        _wavProcessEffects.value = enabled
        NativeEngine.getInstance().setWavBypassChain(!enabled)
    }

    fun setPluginFilePath(pluginIndex: Int, propertyUri: String, filePath: String) {
        viewModelScope.launch {
            try {
                com.varcain.guitarrackcraft.engine.NativeEngine.getInstance()
                    .setPluginFilePath(pluginIndex, propertyUri, filePath)
            } catch (e: Exception) {
                _errorMessage.value = "Failed to set file path: ${e.message}"
            }
        }
    }

    fun setParameter(pluginIndex: Int, portIndex: Int, value: Float) {
        viewModelScope.launch {
            try {
                RackManager.setParameter(pluginIndex, portIndex, value)
            } catch (e: Exception) {
                _errorMessage.value = "Failed to set parameter: ${e.message}"
            }
        }
    }

    fun getParameter(pluginIndex: Int, portIndex: Int): Float {
        return try {
            RackManager.getParameter(pluginIndex, portIndex)
        } catch (e: Exception) {
            0.0f
        }
    }

    /**
     * Returns the UI type to use for this plugin: last user choice if stored and still available,
     * otherwise the default (MODGUI > Native > Sliders).
     */
    fun getPreferredUiTypeForPlugin(pluginInfo: PluginInfo): UiType {
        val ctx = getApplication<Application>()
        val stored = PluginUiPreferenceManager.getStoredUiType(ctx, pluginInfo.fullId)
        return if (stored != null && pluginInfo.guiTypes.contains(stored)) stored
        else pluginInfo.preferredUiType
    }

    /** Persists the user's UI choice for this plugin so it is used when the plugin is added again. */
    fun setPreferredUiTypeForPlugin(pluginFullId: String, uiType: UiType) {
        PluginUiPreferenceManager.setStoredUiType(getApplication<Application>(), pluginFullId, uiType)
    }

    private fun updateRackState() {
        viewModelScope.launch {
            try {
                val plugins = RackManager.getRackPlugins()
                val oldList = _rackPlugins.value
                // Preserve instanceIds for plugins that stayed in the rack
                // Match by pluginId, consuming old entries to handle duplicates
                val availableOld = oldList.groupBy { it.pluginId }.mapValues { it.value.toMutableList() }
                _rackPlugins.value = plugins.mapIndexed { index, pluginInfo ->
                    val fullId = pluginInfo.fullId
                    val existing = availableOld[fullId]?.removeFirstOrNull()
                    RackPlugin(
                        index = index,
                        name = pluginInfo.name.ifEmpty { pluginInfo.id },
                        pluginId = fullId,
                        instanceId = existing?.instanceId ?: RackPlugin.nextInstanceId()
                    )
                }
                android.util.Log.i("AudioLifecycle", "updateRackState: ok size=${plugins.size}")
            } catch (e: Exception) {
                _errorMessage.value = "Failed to get rack plugins: ${e.message}"
                android.util.Log.e("AudioLifecycle", "updateRackState: failed (keeping previous list to avoid tearing down X11 UIs): ${e.message}", e)
            }
        }
    }

    private fun ensureRecentManager(ctx: Context): RecentPresetsManager {
        return recentPresetsManager ?: RecentPresetsManager(ctx).also { recentPresetsManager = it }
    }

    fun refreshPresets(ctx: Context) {
        _presetList.value = presetManager.listPresets(ctx)
        _recentPresets.value = ensureRecentManager(ctx).getRecents()
    }

    fun savePreset(ctx: Context, name: String) {
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) {
                presetManager.savePreset(ctx, name)
            }
            if (ok) {
                ensureRecentManager(ctx).addRecent(name)
                refreshPresets(ctx)
                _presetMessage.value = "Preset '$name' saved"
            } else {
                _presetMessage.value = "Failed to save preset"
            }
        }
    }

    fun loadPreset(ctx: Context, name: String) {
        viewModelScope.launch {
            val engine = NativeEngine.getInstance()
            val ok = withContext(Dispatchers.IO) {
                engine.setChainBypass(true)
                try {
                    presetManager.loadPreset(ctx, name)
                } finally {
                    engine.setChainBypass(false)
                }
            }
            if (ok) {
                ensureRecentManager(ctx).addRecent(name)
                refreshPresets(ctx)
                refreshRack()
                _presetMessage.value = "Preset '$name' loaded"
            } else {
                _presetMessage.value = "Failed to load preset (plugin count mismatch?)"
            }
        }
    }

    fun deletePreset(ctx: Context, name: String) {
        viewModelScope.launch {
            presetManager.deletePreset(ctx, name)
            ensureRecentManager(ctx).removeRecent(name)
            refreshPresets(ctx)
            _presetMessage.value = "Preset '$name' deleted"
        }
    }

    fun getPresetJson(ctx: Context, name: String): String? {
        return presetManager.getPresetJson(ctx, name)
    }

    fun clearRecentPresets(ctx: Context) {
        ensureRecentManager(ctx).clearRecents()
        _recentPresets.value = emptyList()
    }

    fun clearError() {
        _errorMessage.value = null
    }

    fun clearPresetMessage() {
        _presetMessage.value = null
    }

    override fun onCleared() {
        super.onCleared()
    }
}
