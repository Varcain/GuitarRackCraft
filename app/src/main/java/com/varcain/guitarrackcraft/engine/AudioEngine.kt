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
 * Facade for audio engine lifecycle, metering, and latency.
 */
object AudioEngine {
    private val native get() = NativeEngine.getInstance()

    fun start(sampleRate: Float = 48000f, inputDeviceId: Int = 0, outputDeviceId: Int = 0, bufferFrames: Int = 0): Boolean =
        native.startEngine(sampleRate, inputDeviceId, outputDeviceId, bufferFrames)
    fun stop() = native.stopEngine()
    fun isRunning(): Boolean = native.isEngineRunning()
    fun getSampleRate(): Float = native.getSampleRate()
    fun getBufferFrameCount(): Int = native.getBufferFrameCount()
    fun getStreamInfo(): AudioStreamInfo = native.getStreamInfo()
    fun getLatencyMs(): Double = native.getLatencyMs()

    fun getInputLevel(): Float = native.getInputLevel()
    fun getOutputLevel(): Float = native.getOutputLevel()
    fun getCpuLoad(): Float = native.getCpuLoad()
    fun getXRunCount(): Int = native.getXRunCount()
    fun isInputClipping(): Boolean = native.isInputClipping()
    fun isOutputClipping(): Boolean = native.isOutputClipping()
    fun resetClipping() = native.resetClipping()
}
