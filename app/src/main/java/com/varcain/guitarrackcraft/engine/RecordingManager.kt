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
import java.io.RandomAccessFile
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

data class RecordingEntry(
    val timestamp: String,
    val displayName: String,
    val rawFile: File,
    val processedFile: File,
    val durationSec: Double
)

object RecordingManager {

    private const val DIR_NAME = "recordings"
    private val fileTimestampFormat = SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.US)
    private val displayFormat = SimpleDateFormat("MMM dd, yyyy h:mm:ss a", Locale.US)

    private fun recordingsDir(context: Context): File {
        val dir = File(context.filesDir, DIR_NAME)
        dir.mkdirs()
        return dir
    }

    fun startRecording(context: Context): Boolean {
        val engine = NativeEngine.getInstance()
        if (!engine.isEngineRunning()) return false

        val dir = recordingsDir(context)
        val ts = fileTimestampFormat.format(Date())
        val rawPath = File(dir, "Raw_$ts.wav").absolutePath
        val processedPath = File(dir, "Processed_$ts.wav").absolutePath
        return engine.startRecording(rawPath, processedPath)
    }

    fun stopRecording() {
        NativeEngine.getInstance().stopRecording()
    }

    fun isRecording(): Boolean = NativeEngine.getInstance().isRecording()

    fun getRecordingDurationSec(): Double = NativeEngine.getInstance().getRecordingDurationSec()

    fun listRecordings(context: Context): List<RecordingEntry> {
        val dir = recordingsDir(context)
        if (!dir.exists()) return emptyList()

        val rawFiles = dir.listFiles { f -> f.name.startsWith("Raw_") && f.name.endsWith(".wav") }
            ?.sortedByDescending { it.name }
            ?: return emptyList()

        return rawFiles.mapNotNull { rawFile ->
            val ts = rawFile.name.removePrefix("Raw_").removeSuffix(".wav")
            val processedFile = File(dir, "Processed_$ts.wav")
            if (!processedFile.exists()) return@mapNotNull null

            val displayName = try {
                val date = fileTimestampFormat.parse(ts)
                if (date != null) displayFormat.format(date) else ts
            } catch (_: Exception) { ts }

            val duration = getWavDurationFromHeader(rawFile)

            RecordingEntry(
                timestamp = ts,
                displayName = displayName,
                rawFile = rawFile,
                processedFile = processedFile,
                durationSec = duration
            )
        }
    }

    fun deleteRecording(entry: RecordingEntry) {
        entry.rawFile.delete()
        entry.processedFile.delete()
    }

    private fun readLeU16(raf: RandomAccessFile): Int {
        val b0 = raf.read()
        val b1 = raf.read()
        return (b1 shl 8) or b0
    }

    private fun readLeU32(raf: RandomAccessFile): Long {
        val b0 = raf.read().toLong()
        val b1 = raf.read().toLong()
        val b2 = raf.read().toLong()
        val b3 = raf.read().toLong()
        return (b3 shl 24) or (b2 shl 16) or (b1 shl 8) or b0
    }

    private fun getWavDurationFromHeader(file: File): Double {
        return try {
            RandomAccessFile(file, "r").use { raf ->
                if (raf.length() < 44) return 0.0
                raf.seek(22)
                val numChannels = readLeU16(raf)
                val sampleRate = readLeU32(raf)      // offset 24
                raf.seek(34)
                val bitsPerSample = readLeU16(raf)
                raf.seek(40)
                val dataSize = readLeU32(raf)

                if (sampleRate == 0L || numChannels == 0 || bitsPerSample == 0) return 0.0
                val bytesPerFrame = numChannels.toLong() * (bitsPerSample.toLong() / 8L)
                if (bytesPerFrame == 0L) return 0.0
                val totalFrames = dataSize / bytesPerFrame
                totalFrames.toDouble() / sampleRate.toDouble()
            }
        } catch (_: Exception) {
            0.0
        }
    }
}
