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

/**
 * Persists the last UI type (Native / Modgui / Sliders) chosen by the user per plugin.
 * Keyed by [PluginInfo.fullId]. When a plugin is added again later, the app uses this
 * stored UI if it is still available for that plugin.
 */
object PluginUiPreferenceManager {
    private const val PREFS_NAME = "plugin_ui_preferences"
    private const val KEY_PREFIX = "ui_"

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun getStoredUiType(context: Context, pluginFullId: String): UiType? {
        val name = prefs(context).getString("$KEY_PREFIX$pluginFullId", null) ?: return null
        return try {
            UiType.valueOf(name)
        } catch (_: IllegalArgumentException) {
            null
        }
    }

    fun setStoredUiType(context: Context, pluginFullId: String, uiType: UiType) {
        prefs(context).edit().putString("$KEY_PREFIX$pluginFullId", uiType.name).apply()
    }
}
