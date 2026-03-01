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
import android.util.Base64
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * Manages preset save/load using JSON files stored in filesDir/presets/.
 * Each preset captures the full chain state (all plugins' control ports + state properties).
 */
class PresetManager(private val engine: NativeEngine) {

    companion object {
        private const val TAG = "PresetManager"
        private const val PRESETS_DIR = "presets"
    }

    private fun presetsDir(context: Context): File {
        val dir = File(context.filesDir, PRESETS_DIR)
        if (!dir.exists()) dir.mkdirs()
        return dir
    }

    /**
     * Save the current chain state as a named preset.
     * @return true if saved successfully.
     */
    fun savePreset(context: Context, name: String): Boolean {
        val stateJson = engine.saveChainState()
        if (stateJson == null) {
            Log.e(TAG, "savePreset: nativeSaveChainState returned null")
            return false
        }

        // Parse the native JSON and add metadata
        val root = JSONObject(stateJson)
        root.put("presetName", name)
        root.put("timestamp", System.currentTimeMillis())

        // Collect plugin URIs for quick identification
        val plugins = root.optJSONArray("plugins")
        if (plugins != null) {
            val uris = JSONArray()
            for (i in 0 until plugins.length()) {
                uris.put(plugins.getJSONObject(i).optString("uri", ""))
            }
            root.put("pluginUris", uris)
        }

        val file = File(presetsDir(context), "$name.json")
        file.writeText(root.toString(2))
        Log.i(TAG, "savePreset: saved '$name' to ${file.absolutePath}")
        return true
    }

    /**
     * Load a preset by name: reads the file and delegates to [loadPresetFromJson].
     * @return true if all plugins restored successfully.
     */
    fun loadPreset(context: Context, name: String): Boolean {
        val file = File(presetsDir(context), "$name.json")
        if (!file.exists()) {
            Log.e(TAG, "loadPreset: file not found: ${file.absolutePath}")
            return false
        }
        return loadPresetFromJson(file.readText())
    }

    /**
     * Load a preset from a raw JSON string: clears the current rack, adds plugins by URI,
     * then restores their control port values and state properties.
     * @return true if all plugins restored successfully.
     */
    fun loadPresetFromJson(json: String): Boolean {
        val root = JSONObject(json)
        val plugins = root.optJSONArray("plugins")
        if (plugins == null) {
            Log.e(TAG, "loadPresetFromJson: no plugins array in preset")
            return false
        }

        // Clear current rack (remove in reverse order to keep indices valid)
        val rackSize = engine.getRackSize()
        for (i in (rackSize - 1) downTo 0) {
            engine.removePluginFromRack(i)
        }

        // Add each plugin by URI
        for (i in 0 until plugins.length()) {
            val uri = plugins.getJSONObject(i).optString("uri", "")
            if (uri.isEmpty()) {
                Log.e(TAG, "loadPresetFromJson: plugin[$i] has no URI")
                return false
            }
            val fullId = "LV2:$uri"
            val pos = engine.addPluginToRack(fullId, -1)
            if (pos < 0) {
                Log.e(TAG, "loadPresetFromJson: failed to add plugin '$fullId'")
                return false
            }
        }

        // Restore parameters for each plugin
        var allOk = true
        for (i in 0 until plugins.length()) {
            val pluginObj = plugins.getJSONObject(i)
            if (!restorePluginFromJson(i, pluginObj)) {
                allOk = false
            }
        }

        Log.i(TAG, "loadPresetFromJson: restored, allOk=$allOk")
        return allOk
    }

    /**
     * List all saved presets.
     * @return list of preset names (without .json extension).
     */
    fun listPresets(context: Context): List<String> {
        val dir = presetsDir(context)
        return dir.listFiles { f -> f.extension == "json" }
            ?.map { it.nameWithoutExtension }
            ?.sorted()
            ?: emptyList()
    }

    /**
     * Delete a saved preset.
     * @return true if deleted.
     */
    fun deletePreset(context: Context, name: String): Boolean {
        val file = File(presetsDir(context), "$name.json")
        val ok = file.delete()
        Log.i(TAG, "deletePreset: '$name' deleted=$ok")
        return ok
    }

    fun getPresetJson(context: Context, name: String): String? {
        val file = File(presetsDir(context), "$name.json")
        return if (file.exists()) file.readText() else null
    }

    private fun restorePluginFromJson(pluginIndex: Int, pluginObj: JSONObject): Boolean {
        // Control ports
        val controlPortsArr = pluginObj.optJSONArray("controlPorts")
        val portCount = controlPortsArr?.length() ?: 0
        val portValues = FloatArray(portCount)
        val portIndices = IntArray(portCount)
        for (i in 0 until portCount) {
            val cp = controlPortsArr!!.getJSONObject(i)
            portIndices[i] = cp.getInt("index")
            portValues[i] = cp.getDouble("value").toFloat()
        }

        // State properties
        val propsArr = pluginObj.optJSONArray("stateProperties")
        val propCount = propsArr?.length() ?: 0
        val propKeys = Array(propCount) { "" }
        val propTypes = Array(propCount) { "" }
        val propValues = Array(propCount) { ByteArray(0) }
        val propFlags = IntArray(propCount)
        for (i in 0 until propCount) {
            val prop = propsArr!!.getJSONObject(i)
            propKeys[i] = prop.getString("key")
            propTypes[i] = prop.getString("type")
            propFlags[i] = prop.optInt("flags", 0)

            val encoding = prop.optString("encoding", "")
            val valueStr = prop.optString("value", "")
            propValues[i] = if (encoding == "base64") {
                Base64.decode(valueStr, Base64.DEFAULT)
            } else {
                // String value — add null terminator for LV2 compatibility
                (valueStr + "\u0000").toByteArray(Charsets.UTF_8)
            }
        }

        return engine.restorePluginState(
            pluginIndex, portValues, portIndices,
            propKeys, propTypes, propValues, propFlags
        )
    }
}
