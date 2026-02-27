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

package com.varcain.guitarrackcraft.ui.tone3000

import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.receiveAsFlow

object Tone3000CallbackHandler {
    private val _apiKeyChannel = Channel<String>(capacity = Channel.BUFFERED)
    val apiKeyFlow = _apiKeyChannel.receiveAsFlow()

    private val _toneUrlChannel = Channel<String>(capacity = Channel.BUFFERED)
    val toneUrlFlow = _toneUrlChannel.receiveAsFlow()

    fun onApiKeyReceived(apiKey: String) {
        _apiKeyChannel.trySend(apiKey)
    }

    fun onToneUrlReceived(toneUrl: String) {
        _toneUrlChannel.trySend(toneUrl)
    }
}
