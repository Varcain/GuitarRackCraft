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

import java.io.File
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.asSharedFlow

data class ModelLoadedEvent(val pluginIndex: Int, val modelName: String)

/**
 * Facade for plugin rack management: add/remove/reorder plugins, parameters.
 */
object RackManager {
    private val native get() = NativeEngine.getInstance()

    private val _modelLoadedEvents = MutableSharedFlow<ModelLoadedEvent>(extraBufferCapacity = 1)
    val modelLoadedEvents = _modelLoadedEvents.asSharedFlow()

    fun notifyModelLoaded(pluginIndex: Int, modelName: String) {
        _modelLoadedEvents.tryEmit(ModelLoadedEvent(pluginIndex, modelName))
    }

    fun getAvailablePlugins(): List<PluginInfo> = native.getAvailablePlugins()

    fun addPlugin(pluginId: String, position: Int = -1): Int =
        native.addPluginToRack(pluginId, position)

    fun removePlugin(position: Int): Boolean = native.removePluginFromRack(position)

    fun reorder(fromPos: Int, toPos: Int): Boolean = native.reorderRack(fromPos, toPos)

    fun setParameter(pluginIndex: Int, portIndex: Int, value: Float) =
        native.setParameter(pluginIndex, portIndex, value)

    fun getParameter(pluginIndex: Int, portIndex: Int): Float =
        native.getParameter(pluginIndex, portIndex)

    fun getRackSize(): Int = native.getRackSize()

    fun getRackPluginInfo(index: Int): PluginInfo? = native.getRackPluginInfo(index)

    fun getRackPlugins(): List<PluginInfo> = native.getRackPlugins()

    fun processFile(inputFile: File, outputFile: File): Boolean =
        native.processFile(inputFile, outputFile)
}
