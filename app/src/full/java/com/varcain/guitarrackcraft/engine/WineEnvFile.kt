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
import android.util.Log
import java.io.File

/**
 * Owns the one renderer-related line of `<cacheDir>/wine_env.txt`, the KEY=VALUE
 * file the wine host reads (WineHostProcess.cpp: vstpocWineEnvValue /
 * vstpocLavapipeEnv). The native side forces software lavapipe iff
 * `VSTPOC_FORCE_LAVAPIPE=1` is present; absent (or any other value) ⇒ Turnip.
 *
 * The file may also hold unrelated developer lines (e.g. `WINEDEBUG=...`), so the
 * writer rewrites only the `VSTPOC_FORCE_LAVAPIPE` line and preserves everything
 * else verbatim.
 */
object WineEnvFile {
    private const val TAG = "WineEnvFile"
    private const val FILE = "wine_env.txt"
    private const val KEY = "VSTPOC_FORCE_LAVAPIPE"

    /**
     * Make `wine_env.txt` reflect [renderer]: LAVAPIPE ⇒ `VSTPOC_FORCE_LAVAPIPE=1`;
     * TURNIP ⇒ the key removed (absence = Turnip). All other lines are preserved.
     * Should be called off the main thread.
     */
    fun applyRenderer(context: Context, renderer: Renderer) {
        val file = File(context.cacheDir, FILE)

        val kept = if (file.exists()) {
            runCatching {
                file.readLines().filter { it.substringBefore('=').trim() != KEY }
            }.getOrElse {
                Log.w(TAG, "read $FILE failed: $it")
                return
            }
        } else emptyList()

        val lines = kept.toMutableList()
        if (renderer == Renderer.LAVAPIPE) lines.add("$KEY=1")

        // Don't create an empty file that didn't exist before (Turnip default).
        if (lines.isEmpty() && !file.exists()) return

        val content = if (lines.isEmpty()) "" else lines.joinToString("\n") + "\n"
        runCatching {
            val tmp = File(context.cacheDir, "$FILE.tmp")
            tmp.writeText(content)
            if (!tmp.renameTo(file)) {
                file.writeText(content)
                tmp.delete()
            }
        }.onFailure { Log.w(TAG, "write $FILE failed: $it"); return }

        Log.i(TAG, "applyRenderer=$renderer (forceLavapipe=${renderer == Renderer.LAVAPIPE})")
    }
}
