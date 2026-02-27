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

package com.varcain.guitarrackcraft.engine

import android.content.Context
import java.io.File

data class AudioStreamInfo(
    val isAAudio: Boolean = false,
    val inputExclusive: Boolean = false,
    val outputExclusive: Boolean = false,
    val inputLowLatency: Boolean = false,
    val outputLowLatency: Boolean = false,
    val outputMMap: Boolean = false,
    val outputCallback: Boolean = false,
    val framesPerBurst: Int = 0
)

/**
 * JNI bridge to the native audio engine.
 * Provides a Kotlin interface to the C++ audio processing engine.
 */
class NativeEngine private constructor() {
    
    companion object {
        @Volatile
        private var INSTANCE: NativeEngine? = null

        fun getInstance(): NativeEngine {
            return INSTANCE ?: synchronized(this) {
                INSTANCE ?: NativeEngine().also { INSTANCE = it }
            }
        }
    }

    init {
        // Preload libc++_shared.so so that dlopen() of LV2 plugin .so files
        // (which depend on it) can resolve it from the app's lib/ directory
        try {
            System.loadLibrary("c++_shared")
        } catch (_: UnsatisfiedLinkError) {}

        // Preload lilv shared library when using shared LV2 build (liblilv-0.so.0 in jniLibs).
        // With the default static LV2 build (build_all_lv2.sh), lilv is linked into libguitarrackcraft.so
        // and no .so is packaged — preload will fail; that is expected and not an error.
        try {
            System.loadLibrary("lilv-0")
        } catch (e: UnsatisfiedLinkError) {
            android.util.Log.d("NativeEngine", "Lilv preload skipped (static build or no liblilv-0.so): ${e.message}")
        }
        System.loadLibrary("guitarrackcraft")
    }

    /**
     * Set the path where LV2 bundles (e.g. Guitarix) are extracted.
     * Must be called before nativeInit() so plugins are discovered.
     */
    external fun nativeSetLv2Path(path: String)

    /**
     * Set the path to the app's native library directory (for symlink-based plugin loading).
     * Must be called before nativeInit().
     */
    external fun nativeSetNativeLibDir(path: String)

    /**
     * Set the path to the app's files directory. Call before nativeInit() if needed.
     */
    external fun nativeSetFilesDir(path: String)

    /**
     * Set the path to extracted X11 SONAME libs (libX11.so.6 etc.) so plugin UI load can copy them into the bundle dir.
     * Call after extracting assets to e.g. context.filesDir/x11_libs.
     */
    external fun nativeSetX11LibsDir(path: String)

    /**
     * Set the path to extracted PAD plugin .so files (playstore flavor).
     * Must be called before nativeInit() so plugins are discovered.
     */
    external fun nativeSetPluginLibDir(path: String)

    /**
     * Initialize the native engine.
     * Must be called before any other methods.
     * Call nativeSetLv2Path() first with the extracted assets path to enable LV2 plugins.
     */
    external fun nativeInit(): Boolean

    /**
     * Start the audio engine.
     * @param sampleRate Desired sample rate (will use device default if not supported)
     * @return true if started successfully
     */
    external fun nativeStartEngine(sampleRate: Float = 48000f, inputDeviceId: Int = 0, outputDeviceId: Int = 0, bufferFrames: Int = 0): Boolean

    /**
     * Stop the audio engine.
     */
    external fun nativeStopEngine()

    /**
     * Check if the engine is currently running.
     */
    external fun nativeIsEngineRunning(): Boolean

    /**
     * Get current sample rate in Hz.
     */
    external fun nativeGetSampleRate(): Float

    /**
     * Get actual buffer frame count used by the audio callback.
     */
    external fun nativeGetBufferFrameCount(): Int

    /**
     * Get stream info for low-latency checklist.
     * Returns [isAAudio, inputExclusive, outputExclusive, inputLowLatency, outputLowLatency, outputMMap, outputCallback, framesPerBurst]
     */
    external fun nativeGetStreamInfo(): IntArray

    /**
     * Get current audio latency in milliseconds.
     */
    external fun nativeGetLatencyMs(): Double

    /**
     * Get input peak level (0.0–1.0).
     */
    external fun nativeGetInputLevel(): Float

    /**
     * Get output peak level (0.0–1.0).
     */
    external fun nativeGetOutputLevel(): Float

    /**
     * Get CPU load (0.0–1.0).
     */
    external fun nativeGetCpuLoad(): Float

    /**
     * Get cumulative audio xrun (underrun/overrun) count.
     */
    external fun nativeGetXRunCount(): Int

    /**
     * True if input has clipped.
     */
    external fun nativeIsInputClipping(): Boolean

    /**
     * True if output has clipped.
     */
    external fun nativeIsOutputClipping(): Boolean

    /**
     * Clear clipping indicators.
     */
    external fun nativeResetClipping()

    /**
     * Get list of all available plugins.
     */
    external fun nativeGetAvailablePlugins(): Array<PluginInfo>

    /**
     * Add a plugin to the rack.
     * @param pluginId Full plugin ID (format:plugin_id)
     * @param position Position in chain (-1 for end)
     * @return Index of inserted plugin, or -1 on failure
     */
    external fun nativeAddPluginToRack(pluginId: String, position: Int = -1): Int

    /**
     * Remove a plugin from the rack.
     * @param position Index of plugin to remove
     * @return true if removed successfully
     */
    external fun nativeRemovePluginFromRack(position: Int): Boolean

    /**
     * Reorder plugins in the rack.
     * @param fromPos Source position
     * @param toPos Destination position
     * @return true if reordered successfully
     */
    external fun nativeReorderRack(fromPos: Int, toPos: Int): Boolean

    /**
     * Send a file path to a plugin via LV2 patch:Set.
     * @param pluginIndex Index of plugin in chain
     * @param propertyUri LV2 property URI (e.g. model file parameter)
     * @param filePath Absolute file path on device
     */
    external fun nativeSetPluginFilePath(pluginIndex: Int, propertyUri: String, filePath: String)

    /**
     * Set a parameter value for a plugin in the rack.
     * @param pluginIndex Index of plugin in chain
     * @param portIndex Index of control port
     * @param value Parameter value
     */
    external fun nativeSetParameter(pluginIndex: Int, portIndex: Int, value: Float)

    /**
     * Get a parameter value for a plugin in the rack.
     * @param pluginIndex Index of plugin in chain
     * @param portIndex Index of control port
     * @return Current parameter value
     */
    external fun nativeGetParameter(pluginIndex: Int, portIndex: Int): Float

    /**
     * Process an audio file offline through the plugin chain.
     * @param inputPath Path to input WAV file
     * @param outputPath Path to output WAV file
     * @return true if processing succeeded
     */
    external fun nativeProcessFile(inputPath: String, outputPath: String): Boolean

    /**
     * Get the number of plugins in the rack.
     */
    external fun nativeGetRackSize(): Int

    /**
     * Get plugin info for a plugin in the rack.
     * @param index Index of plugin in rack
     * @return PluginInfo or null if index is invalid
     */
    external fun nativeGetRackPluginInfo(index: Int): PluginInfo?

    // --- X11 UI management (EGL + ANativeWindow, native X server) ---

    /**
     * Attach an Android Surface to the given X11 display number.
     * Starts the native X server and EGL rendering. Returns root window ID for createPluginUI.
     */
    external fun nativeAttachSurfaceToDisplay(displayNumber: Int, surface: android.view.Surface, width: Int, height: Int): Long

    /** Detach surface and stop X server for the display. */
    /** Signal X11 threads to exit without blocking (call from view destroy callback). */
    /** @return true if detach was deferred (createPluginUI still running); false if connection was closed now */
    external fun nativeSignalDetachSurfaceFromDisplay(displayNumber: Int): Boolean
    external fun nativeStopX11RenderThreadOnly(displayNumber: Int)
    external fun nativeDetachSurfaceFromDisplay(displayNumber: Int)

    /** Release display resources (call after detach when releasing display number). */
    external fun nativeDestroyX11Display(displayNumber: Int)
    /** Detach and destroy display only if it exists (for delayed cleanup from view destroy). */
    external fun nativeDetachAndDestroyX11DisplayIfExists(displayNumber: Int)

    /**
     * Hide the X11 display without destroying it.
     * Stops the render thread, but keeps the X server and surface alive.
     * This allows the display to be reused when switching back to X11 UI mode.
     * Use this when switching to another UI mode (MODGUI/Sliders) to avoid driver mutex crashes.
     */
    external fun nativeHideX11Display(displayNumber: Int)

    /**
     * Resume the X11 display after hiding.
     * Restarts the render thread to resume rendering.
     * Use this when switching back to X11 UI mode.
     */
    external fun nativeResumeX11Display(displayNumber: Int)

    /** Update surface size (e.g. on surfaceChanged). */
    external fun nativeSetSurfaceSize(displayNumber: Int, width: Int, height: Int)

    /** Inject touch: action 0=down, 1=up, 2=move; x, y in view coordinates. */
    external fun nativeInjectTouch(displayNumber: Int, action: Int, x: Int, y: Int)

    /** Hit-test: returns true if (x, y) in surface coords hits an X11 widget (knob, slider, etc.). */
    external fun nativeIsWidgetAtPoint(displayNumber: Int, x: Int, y: Int): Boolean

    /** Request a frame (swap buffers). */
    external fun nativeRequestX11Frame(displayNumber: Int)

    /** Get plugin natural window size [width, height]. Returns [0, 0] if not yet known. */
    external fun nativeGetX11PluginSize(displayNumber: Int): IntArray

    /** Get the UI scale factor for the given display (1.0 = no scaling). */
    external fun nativeGetX11UIScale(displayNumber: Int): Float

    /**
     * Mark that we are about to create a plugin UI for this display.
     * Call from the UI thread before posting createPluginUI to the executor, so that if surfaceDestroyed
     * runs before the executor runs, signalDetach will defer (avoid closing X11 connection and killing the process).
     */
    external fun nativeBeginCreatePluginUI(displayNumber: Int, pluginIndex: Int)

    /**
     * Create an X11 UI for the plugin at the given rack index.
     * The UI binary is loaded via dlopen and connects to the native X server; parentWindowId is the root window ID from nativeAttachSurfaceToDisplay.
     */
    external fun nativeCreatePluginUI(pluginIndex: Int, displayNumber: Int, parentWindowId: Long): Boolean

    /** Tear down the X11 UI for the given rack index. */
    external fun nativeDestroyPluginUI(pluginIndex: Int)

    /** Pump idle on every active X11 UI (event processing).  Returns true if any UI reported work (dirty). */
    external fun nativeIdlePluginUIs(): Boolean

    /** Poll for a pending file request from any X11 plugin UI (ui:requestValue).
     *  Returns [pluginIndex, propertyUri] or null if no request pending. */
    external fun nativePollFileRequest(): Array<String>?

    /** Deliver a file path to a plugin's X11 UI via patch:Set atom. */
    external fun nativeDeliverFileToPluginUI(pluginIndex: Int, propertyUri: String, filePath: String)

    /** Save the complete chain state (all plugins) as a JSON string. */
    external fun nativeSaveChainState(): String?

    /** Restore a single plugin's state from decomposed arrays. */
    external fun nativeRestorePluginState(
        pluginIndex: Int,
        portValues: FloatArray, portIndices: IntArray,
        propertyKeys: Array<String>, propertyTypes: Array<String>,
        propertyValues: Array<ByteArray>, propertyFlags: IntArray
    ): Boolean

    // Kotlin wrappers
    fun attachSurfaceToDisplay(displayNumber: Int, surface: android.view.Surface, width: Int, height: Int): Long =
        nativeAttachSurfaceToDisplay(displayNumber, surface, width, height)

    fun signalDetachSurfaceFromDisplay(displayNumber: Int): Boolean = nativeSignalDetachSurfaceFromDisplay(displayNumber)
    fun stopX11RenderThreadOnly(displayNumber: Int) = nativeStopX11RenderThreadOnly(displayNumber)
    fun detachSurfaceFromDisplay(displayNumber: Int) = nativeDetachSurfaceFromDisplay(displayNumber)
    fun destroyX11Display(displayNumber: Int) = nativeDestroyX11Display(displayNumber)
    fun detachAndDestroyX11DisplayIfExists(displayNumber: Int) = nativeDetachAndDestroyX11DisplayIfExists(displayNumber)
    fun hideX11Display(displayNumber: Int) = nativeHideX11Display(displayNumber)
    fun resumeX11Display(displayNumber: Int) = nativeResumeX11Display(displayNumber)
    fun setSurfaceSize(displayNumber: Int, width: Int, height: Int) = nativeSetSurfaceSize(displayNumber, width, height)
    fun injectTouch(displayNumber: Int, action: Int, x: Int, y: Int) = nativeInjectTouch(displayNumber, action, x, y)
    fun isWidgetAtPoint(displayNumber: Int, x: Int, y: Int): Boolean = nativeIsWidgetAtPoint(displayNumber, x, y)
    fun requestX11Frame(displayNumber: Int) = nativeRequestX11Frame(displayNumber)
    fun getX11PluginSize(displayNumber: Int): IntArray = nativeGetX11PluginSize(displayNumber)
    fun getX11UIScale(displayNumber: Int): Float = nativeGetX11UIScale(displayNumber)

    /**
     * Ensure the X11 scratch directory exists and set on native side.
     * X11 SONAME libs (libX11.so.6 etc.) are loaded from nativeLibDir at runtime.
     * This directory is used as a writable scratch dir for plugin UI .so copies.
     */
    fun ensureX11LibsDir(context: Context): File {
        val dir = File(context.filesDir, "x11_libs/arm64-v8a")
        dir.mkdirs()
        nativeSetX11LibsDir(dir.absolutePath)
        return dir
    }

    fun createPluginUI(pluginIndex: Int, displayNumber: Int, parentWindowId: Long): Boolean =
        nativeCreatePluginUI(pluginIndex, displayNumber, parentWindowId)

    fun destroyPluginUI(pluginIndex: Int) = nativeDestroyPluginUI(pluginIndex)

    fun idlePluginUIs(): Boolean = nativeIdlePluginUIs()

    // --- Real-time recording ---

    external fun nativeStartRecording(rawPath: String, processedPath: String): Boolean
    external fun nativeStopRecording()
    external fun nativeIsRecording(): Boolean
    external fun nativeGetRecordingDurationSec(): Double

    // --- WAV real-time playback ---

    external fun nativeLoadWav(path: String): Boolean
    external fun nativeUnloadWav()
    external fun nativeWavPlay()
    external fun nativeWavPause()
    external fun nativeWavSeek(positionSec: Double)
    external fun nativeGetWavDurationSec(): Double
    external fun nativeGetWavPositionSec(): Double
    external fun nativeIsWavPlaying(): Boolean
    external fun nativeIsWavLoaded(): Boolean
    external fun nativeSetChainBypass(bypass: Boolean)
    external fun nativeSetWavBypassChain(bypass: Boolean)

    // Kotlin-friendly wrapper methods
    fun setChainBypass(bypass: Boolean) = nativeSetChainBypass(bypass)
    fun setWavBypassChain(bypass: Boolean) = nativeSetWavBypassChain(bypass)

    fun startEngine(sampleRate: Float = 48000f, inputDeviceId: Int = 0, outputDeviceId: Int = 0, bufferFrames: Int = 0): Boolean {
        return nativeStartEngine(sampleRate, inputDeviceId, outputDeviceId, bufferFrames)
    }

    fun stopEngine() {
        nativeStopEngine()
    }

    fun isEngineRunning(): Boolean {
        return nativeIsEngineRunning()
    }

    fun getSampleRate(): Float = nativeGetSampleRate()
    fun getBufferFrameCount(): Int = nativeGetBufferFrameCount()
    fun getStreamInfo(): AudioStreamInfo {
        val arr = nativeGetStreamInfo()
        return AudioStreamInfo(
            isAAudio = arr[0] != 0,
            inputExclusive = arr[1] != 0,
            outputExclusive = arr[2] != 0,
            inputLowLatency = arr[3] != 0,
            outputLowLatency = arr[4] != 0,
            outputMMap = arr[5] != 0,
            outputCallback = arr[6] != 0,
            framesPerBurst = arr[7]
        )
    }

    fun getLatencyMs(): Double {
        return nativeGetLatencyMs()
    }

    fun getInputLevel(): Float = nativeGetInputLevel()
    fun getOutputLevel(): Float = nativeGetOutputLevel()
    fun getCpuLoad(): Float = nativeGetCpuLoad()
    fun getXRunCount(): Int = nativeGetXRunCount()
    fun isInputClipping(): Boolean = nativeIsInputClipping()
    fun isOutputClipping(): Boolean = nativeIsOutputClipping()
    fun resetClipping() = nativeResetClipping()

    fun getAvailablePlugins(): List<PluginInfo> {
        return nativeGetAvailablePlugins().toList()
    }

    fun addPluginToRack(pluginId: String, position: Int = -1): Int {
        return nativeAddPluginToRack(pluginId, position)
    }

    fun removePluginFromRack(position: Int): Boolean {
        return nativeRemovePluginFromRack(position)
    }

    fun reorderRack(fromPos: Int, toPos: Int): Boolean {
        return nativeReorderRack(fromPos, toPos)
    }

    fun setPluginFilePath(pluginIndex: Int, propertyUri: String, filePath: String) {
        nativeSetPluginFilePath(pluginIndex, propertyUri, filePath)
    }

    fun setParameter(pluginIndex: Int, portIndex: Int, value: Float) {
        nativeSetParameter(pluginIndex, portIndex, value)
    }

    fun getParameter(pluginIndex: Int, portIndex: Int): Float {
        return nativeGetParameter(pluginIndex, portIndex)
    }

    fun processFile(inputFile: File, outputFile: File): Boolean {
        return nativeProcessFile(inputFile.absolutePath, outputFile.absolutePath)
    }

    fun getRackSize(): Int {
        return nativeGetRackSize()
    }

    fun getRackPluginInfo(index: Int): PluginInfo? {
        return nativeGetRackPluginInfo(index)
    }

    fun getRackPlugins(): List<PluginInfo> {
        val size = getRackSize()
        return (0 until size).mapNotNull { index ->
            getRackPluginInfo(index)?.apply {
                // Store the index for reference
            }
        }
    }

    // Recording wrappers
    fun startRecording(rawPath: String, processedPath: String): Boolean = nativeStartRecording(rawPath, processedPath)
    fun stopRecording() = nativeStopRecording()
    fun isRecording(): Boolean = nativeIsRecording()
    fun getRecordingDurationSec(): Double = nativeGetRecordingDurationSec()

    // WAV playback wrappers
    fun loadWav(file: File): Boolean = nativeLoadWav(file.absolutePath)
    fun loadWav(path: String): Boolean = nativeLoadWav(path)
    fun unloadWav() = nativeUnloadWav()
    fun wavPlay() = nativeWavPlay()
    fun wavPause() = nativeWavPause()
    fun wavSeek(positionSec: Double) = nativeWavSeek(positionSec)
    fun getWavDurationSec(): Double = nativeGetWavDurationSec()
    fun getWavPositionSec(): Double = nativeGetWavPositionSec()
    fun isWavPlaying(): Boolean = nativeIsWavPlaying()
    fun isWavLoaded(): Boolean = nativeIsWavLoaded()

    // State save/restore wrappers
    fun saveChainState(): String? = nativeSaveChainState()

    fun restorePluginState(
        pluginIndex: Int,
        portValues: FloatArray, portIndices: IntArray,
        propertyKeys: Array<String>, propertyTypes: Array<String>,
        propertyValues: Array<ByteArray>, propertyFlags: IntArray
    ): Boolean = nativeRestorePluginState(
        pluginIndex, portValues, portIndices,
        propertyKeys, propertyTypes, propertyValues, propertyFlags
    )
}
