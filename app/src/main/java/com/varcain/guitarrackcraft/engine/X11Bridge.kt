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

/**
 * Facade for all X11 display/surface/touch/plugin-UI methods.
 */
object X11Bridge {
    private val native get() = NativeEngine.getInstance()

    fun attachSurfaceToDisplay(displayNumber: Int, surface: android.view.Surface, width: Int, height: Int): Long =
        native.attachSurfaceToDisplay(displayNumber, surface, width, height)

    fun signalDetachSurfaceFromDisplay(displayNumber: Int): Boolean =
        native.signalDetachSurfaceFromDisplay(displayNumber)

    fun stopX11RenderThreadOnly(displayNumber: Int) = native.stopX11RenderThreadOnly(displayNumber)
    fun detachSurfaceFromDisplay(displayNumber: Int) = native.detachSurfaceFromDisplay(displayNumber)
    fun destroyX11Display(displayNumber: Int) = native.destroyX11Display(displayNumber)
    fun detachAndDestroyX11DisplayIfExists(displayNumber: Int) = native.detachAndDestroyX11DisplayIfExists(displayNumber)
    fun hideX11Display(displayNumber: Int) = native.hideX11Display(displayNumber)
    fun resumeX11Display(displayNumber: Int) = native.resumeX11Display(displayNumber)
    fun setSurfaceSize(displayNumber: Int, width: Int, height: Int) = native.setSurfaceSize(displayNumber, width, height)
    fun injectTouch(displayNumber: Int, action: Int, x: Int, y: Int) = native.injectTouch(displayNumber, action, x, y)
    fun isWidgetAtPoint(displayNumber: Int, x: Int, y: Int): Boolean = native.isWidgetAtPoint(displayNumber, x, y)
    fun requestX11Frame(displayNumber: Int) = native.requestX11Frame(displayNumber)
    fun getX11PluginSize(displayNumber: Int): IntArray = native.getX11PluginSize(displayNumber)
    fun getX11UIScale(displayNumber: Int): Float = native.getX11UIScale(displayNumber)

    fun ensureX11LibsDir(context: Context): File = native.ensureX11LibsDir(context)

    fun beginCreatePluginUI(displayNumber: Int, pluginIndex: Int) =
        native.nativeBeginCreatePluginUI(displayNumber, pluginIndex)

    fun createPluginUI(pluginIndex: Int, displayNumber: Int, parentWindowId: Long): Boolean =
        native.createPluginUI(pluginIndex, displayNumber, parentWindowId)

    fun destroyPluginUI(pluginIndex: Int) = native.destroyPluginUI(pluginIndex)
    fun idlePluginUIs(): Boolean = native.idlePluginUIs()

    fun pollFileRequest(): Array<String>? = native.nativePollFileRequest()
    fun deliverFileToPluginUI(pluginIndex: Int, propertyUri: String, filePath: String) =
        native.nativeDeliverFileToPluginUI(pluginIndex, propertyUri, filePath)
}
