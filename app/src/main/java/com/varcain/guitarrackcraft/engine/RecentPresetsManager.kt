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

class RecentPresetsManager(context: Context) {

    companion object {
        private const val PREFS_NAME = "recent_presets"
        private const val KEY_RECENTS = "recents"
        private const val MAX_RECENTS = 5
    }

    private val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun getRecents(): List<String> {
        val raw = prefs.getString(KEY_RECENTS, "") ?: ""
        return if (raw.isEmpty()) emptyList() else raw.split("\n")
    }

    fun addRecent(name: String) {
        val list = getRecents().toMutableList()
        list.remove(name)
        list.add(0, name)
        if (list.size > MAX_RECENTS) list.subList(MAX_RECENTS, list.size).clear()
        prefs.edit().putString(KEY_RECENTS, list.joinToString("\n")).apply()
    }

    fun removeRecent(name: String) {
        val list = getRecents().toMutableList()
        list.remove(name)
        prefs.edit().putString(KEY_RECENTS, list.joinToString("\n")).apply()
    }

    fun clearRecents() {
        prefs.edit().putString(KEY_RECENTS, "").apply()
    }
}
