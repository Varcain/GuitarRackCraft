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

/**
 * Facade for WAV real-time playback methods.
 */
object WavPlayer {
    private val native get() = NativeEngine.getInstance()

    fun load(file: File): Boolean = native.loadWav(file)
    fun load(path: String): Boolean = native.loadWav(path)
    fun unload() = native.unloadWav()
    fun play() = native.wavPlay()
    fun pause() = native.wavPause()
    fun seek(positionSec: Double) = native.wavSeek(positionSec)
    fun getDurationSec(): Double = native.getWavDurationSec()
    fun getPositionSec(): Double = native.getWavPositionSec()
    fun isPlaying(): Boolean = native.isWavPlaying()
    fun isLoaded(): Boolean = native.isWavLoaded()
}
