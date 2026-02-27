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

object FavoritesManager {
    private const val PREFS_NAME = "plugin_favorites"
    private const val KEY_FAVORITES = "favorites"

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun getFavorites(context: Context): Set<String> =
        prefs(context).getStringSet(KEY_FAVORITES, emptySet()) ?: emptySet()

    fun isFavorite(context: Context, fullId: String): Boolean =
        getFavorites(context).contains(fullId)

    fun toggleFavorite(context: Context, fullId: String) {
        val current = getFavorites(context).toMutableSet()
        if (!current.add(fullId)) {
            current.remove(fullId)
        }
        prefs(context).edit().putStringSet(KEY_FAVORITES, current).apply()
    }
}
