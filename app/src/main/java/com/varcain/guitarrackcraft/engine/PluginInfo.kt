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

/**
 * One enumeration/scale point for a control port (label + value).
 */
data class ScalePoint(
    val label: String = "",
    val value: Float = 0.0f
)

/**
 * Information about a plugin port (parameter or audio).
 */
data class PortInfo(
    val index: Int = 0,
    val name: String = "",
    val symbol: String = "",
    val isInput: Boolean = true,
    val isAudio: Boolean = false,
    val isControl: Boolean = false,
    val isToggle: Boolean = false,
    val defaultValue: Float = 0.0f,
    val minValue: Float = 0.0f,
    val maxValue: Float = 1.0f,
    val scalePoints: List<ScalePoint> = emptyList()
)

/**
 * Information about an available plugin.
 */
/**
 * Describes the preferred UI rendering mode for a plugin.
 */
enum class UiType(val displayName: String) {
    /** Native X11 UI (xputty/Cairo based). */
    X11("Native"),
    /** Web-based modgui rendered in a WebView. */
    MODGUI("Modgui"),
    /** Plain Compose sliders / toggles (always available). */
    SLIDERS("Sliders")
}

/**
 * Information about an available plugin.
 */
data class PluginInfo(
    val id: String = "",
    val name: String = "",
    val format: String = "",
    val ports: List<PortInfo> = emptyList(),
    /** If non-empty, path to bundle directory for modgui (file:// base for WebView). */
    val modguiBasePath: String = "",
    /** Relative path to modgui icon HTML (e.g. modgui/icon-gxmicroamp.html). Only set if modguiBasePath is set. */
    val modguiIconTemplate: String = "",
    /** True when the plugin ships with an X11UI binary. */
    val hasX11Ui: Boolean = false,
    /** Absolute path to the X11 UI shared library (.so). */
    val x11UiBinaryPath: String = "",
    /** LV2 URI of the X11 UI (from the TTL). */
    val x11UiUri: String = "",
    /** Path to the thumbnail image in assets (e.g., "GxPlugins.lv2/GxVoodooFuzz.lv2/modgui/thumbnail-gxvoodoofuzz.png"). */
    val thumbnailPath: String = "",
    /** Plugin description from README.md (may be empty). */
    val description: String = ""
) {
    val fullId: String
        get() = "$format:$id"
    
    val controlPorts: List<PortInfo>
        get() = ports.filter { it.isControl && !it.isAudio }
    
    val hasModgui: Boolean
        get() = modguiBasePath.isNotEmpty() && modguiIconTemplate.isNotEmpty()

    /** Default UI: MODGUI when available, else Native (X11), else Sliders. Overflow menu order is unchanged (Native, Modgui, Sliders). */
    val preferredUiType: UiType
        get() = when {
            hasModgui -> UiType.MODGUI
            hasX11Ui -> UiType.X11
            else -> UiType.SLIDERS
        }

    /** List of GUI types this plugin exposes (X11, MODGUI, SLIDERS). Sliders are always available. */
    val guiTypes: List<UiType>
        get() = buildList {
            if (hasX11Ui) add(UiType.X11)
            if (hasModgui) add(UiType.MODGUI)
            add(UiType.SLIDERS)
        }
}
