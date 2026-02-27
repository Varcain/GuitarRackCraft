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

import java.io.File

data class ModelFileInfo(
    val isNam: Boolean,
    val isIr: Boolean,
    val isAidaX: Boolean,
    val storageDirName: String,
    val extension: String,
    val toneDirName: String,
    val fileName: String
) {
    fun resolveFile(filesDir: File): File {
        val storageDir = File(filesDir, storageDirName)
        val toneDir = File(storageDir, toneDirName)
        return File(toneDir, fileName)
    }
}

object ToneFileUtils {

    fun classifyModel(tone: Tone, model: Model): ModelFileInfo {
        val isNam = model.platform?.lowercase() == "nam" ||
                model.name.lowercase().contains("nam") ||
                tone.platform?.lowercase() == "nam" ||
                tone.gear?.lowercase()?.contains("nam") == true ||
                tone.title.lowercase().contains("nam")

        val isIr = model.platform?.lowercase() == "ir" || tone.platform?.lowercase() == "ir"
        val isAidaX = model.platform?.lowercase() == "aida-x" || tone.platform?.lowercase() == "aida-x"

        val storageDirName = when {
            isIr -> "ir_models"
            isNam -> "neural_models"
            isAidaX -> "aidax_models"
            else -> "aidax_models"
        }
        val extension = when {
            isIr -> "wav"
            isNam -> "nam"
            else -> "json"
        }

        val toneDirName = tone.title.replace(" ", "_").replace("/", "-")
        val fileName = "${model.name.replace(" ", "_")}.$extension"

        return ModelFileInfo(
            isNam = isNam,
            isIr = isIr,
            isAidaX = isAidaX,
            storageDirName = storageDirName,
            extension = extension,
            toneDirName = toneDirName,
            fileName = fileName
        )
    }

    fun isModelDownloaded(filesDir: File, tone: Tone, model: Model): Boolean {
        val info = classifyModel(tone, model)
        return info.resolveFile(filesDir).exists()
    }

    /**
     * Resolve the LV2 property URI for loading a file into a plugin.
     *
     * @param sourceSlot URI fragment identifying the specific slot (e.g. "Neural_Model1",
     *   "irfile1"). When non-null and targeting NeuralRack, this overrides the default
     *   slot so the file lands in the exact slot the user browsed from.
     */
    fun resolvePropertyUri(fileInfo: ModelFileInfo, pluginId: String?, sourceSlot: String? = null): String {
        val isNeuralRack = pluginId?.contains("neuralrack", ignoreCase = true) == true

        // If an explicit slot was provided and we're targeting NeuralRack, honour it
        if (sourceSlot != null && isNeuralRack) {
            return "urn:brummer:neuralrack#$sourceSlot"
        }

        return when {
            fileInfo.isNam || (fileInfo.isAidaX && pluginId?.contains("neural-amp-modeler") == true) ->
                "http://github.com/mikeoliphant/neural-amp-modeler-lv2#model"
            (fileInfo.isNam || fileInfo.isAidaX) && isNeuralRack ->
                "urn:brummer:neuralrack#Neural_Model"
            fileInfo.isIr && pluginId?.contains("ImpulseLoader") == true ->
                "urn:brummer:ImpulseLoader#irfile"
            fileInfo.isIr && isNeuralRack ->
                "urn:brummer:neuralrack#irfile"
            else -> "http://aidadsp.cc/plugins/aidadsp-bundle/rt-neural-generic#json"
        }
    }
}
