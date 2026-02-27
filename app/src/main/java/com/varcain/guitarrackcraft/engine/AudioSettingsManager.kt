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
import android.media.AudioDeviceInfo
import android.media.AudioManager

data class AudioDeviceOption(
    val id: Int,
    val name: String,
    val type: Int
)

object AudioSettingsManager {
    private const val PREFS_NAME = "audio_settings"
    private const val KEY_INPUT_DEVICE_ID = "inputDeviceId"
    private const val KEY_OUTPUT_DEVICE_ID = "outputDeviceId"
    private const val KEY_BUFFER_SIZE = "bufferSize"

    val BUFFER_SIZE_OPTIONS = listOf(
        0 to "Auto",
        16 to "16",
        32 to "32",
        64 to "64",
        128 to "128",
        256 to "256",
        512 to "512",
        1024 to "1024"
    )

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun getInputDeviceId(context: Context): Int =
        prefs(context).getInt(KEY_INPUT_DEVICE_ID, 0)

    fun setInputDeviceId(context: Context, id: Int) {
        prefs(context).edit().putInt(KEY_INPUT_DEVICE_ID, id).apply()
    }

    fun getOutputDeviceId(context: Context): Int =
        prefs(context).getInt(KEY_OUTPUT_DEVICE_ID, 0)

    fun setOutputDeviceId(context: Context, id: Int) {
        prefs(context).edit().putInt(KEY_OUTPUT_DEVICE_ID, id).apply()
    }

    fun getBufferSize(context: Context): Int =
        prefs(context).getInt(KEY_BUFFER_SIZE, 0)

    fun setBufferSize(context: Context, size: Int) {
        prefs(context).edit().putInt(KEY_BUFFER_SIZE, size).apply()
    }

    fun getInputDevices(context: Context): List<AudioDeviceOption> {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val devices = am.getDevices(AudioManager.GET_DEVICES_INPUTS)
        return buildList {
            add(AudioDeviceOption(0, "Default", 0))
            devices.forEach { dev ->
                val name = dev.productName?.toString()?.ifEmpty { null }
                    ?: deviceTypeName(dev.type)
                add(AudioDeviceOption(dev.id, "$name (${deviceTypeName(dev.type)})", dev.type))
            }
        }
    }

    fun getOutputDevices(context: Context): List<AudioDeviceOption> {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val devices = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        return buildList {
            add(AudioDeviceOption(0, "Default", 0))
            devices.forEach { dev ->
                val name = dev.productName?.toString()?.ifEmpty { null }
                    ?: deviceTypeName(dev.type)
                add(AudioDeviceOption(dev.id, "$name (${deviceTypeName(dev.type)})", dev.type))
            }
        }
    }

    private fun deviceTypeName(type: Int): String = when (type) {
        AudioDeviceInfo.TYPE_BUILTIN_MIC -> "Built-in Mic"
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "Built-in Speaker"
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> "Earpiece"
        AudioDeviceInfo.TYPE_WIRED_HEADSET -> "Wired Headset"
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "Wired Headphones"
        AudioDeviceInfo.TYPE_USB_DEVICE -> "USB Device"
        AudioDeviceInfo.TYPE_USB_ACCESSORY -> "USB Accessory"
        AudioDeviceInfo.TYPE_USB_HEADSET -> "USB Headset"
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> "Bluetooth SCO"
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "Bluetooth A2DP"
        AudioDeviceInfo.TYPE_TELEPHONY -> "Telephony"
        AudioDeviceInfo.TYPE_AUX_LINE -> "Aux Line"
        AudioDeviceInfo.TYPE_HDMI -> "HDMI"
        AudioDeviceInfo.TYPE_HDMI_ARC -> "HDMI ARC"
        else -> "Audio Device"
    }
}
