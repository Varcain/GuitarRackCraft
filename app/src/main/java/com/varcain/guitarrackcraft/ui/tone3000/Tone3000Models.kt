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

import java.io.Serializable

data class DataWrapper<T>(
    val data: T
) : Serializable

data class PaginatedResponse<T>(
    val data: T,
    val page: Int,
    val page_size: Int,
    val total: Int,
    val total_pages: Int
) : Serializable

data class Tone(
    val id: String,
    val title: String,
    val description: String? = null,
    val user: EmbeddedUser? = null,
    val images: List<String>? = null,
    val models_count: Int = 0,
    val downloads_count: Int = 0,
    val favorites_count: Int = 0,
    val url: String,
    val gear: String? = null,
    val sizes: String? = null,
    val platform: String? = null,
    val tags: List<Tag>? = null,
    val models: List<Model>? = null
) : Serializable

data class EmbeddedUser(
    val id: String,
    val username: String,
    val avatar_url: String? = null,
    val url: String
) : Serializable

data class Tag(
    val id: String? = null,
    val name: String
) : Serializable

data class Session(
    val access_token: String,
    val refresh_token: String,
    val expires_in: Int,
    val token_type: String
) : Serializable

data class AuthRequest(
    val api_key: String
) : Serializable

data class RefreshRequest(
    val refresh_token: String,
    val access_token: String
) : Serializable

data class User(
    val id: String,
    val username: String,
    val bio: String? = null,
    val avatar_url: String? = null,
    val url: String
) : Serializable

data class Model(
    val id: String,
    val name: String,
    val size: String,
    val platform: String? = null,
    val model_url: String,
    val created_at: String? = null,
    val updated_at: String? = null
) : Serializable

enum class Platform(val value: String, val displayName: String) {
    NAM("nam", "NAM"),
    IR("ir", "IR"),
    AIDA_X("aida-x", "AIDA-X"),
    AA_SNAPSHOT("aa-snapshot", "A-A Snapshot"),
    PROTEUS("proteus", "Proteus");

    companion object {
        fun fromString(value: String?): String {
            if (value == null) return "Unknown"
            return values().find { it.value.lowercase() == value.lowercase() }?.displayName ?: value.replaceFirstChar { it.uppercase() }
        }
    }
}

enum class Gear(val value: String, val displayName: String) {
    AMP("amp", "Amp"),
    FULL_RIG("full-rig", "Full Rig"),
    PEDAL("pedal", "Pedal"),
    OUTBOARD("outboard", "Outboard"),
    IR("ir", "IR");

    companion object {
        fun fromString(value: String?): String {
            if (value == null) return "Unknown"
            return values().find { it.value.lowercase() == value.lowercase() }?.displayName ?: value.replaceFirstChar { it.uppercase() }
        }
    }
}

enum class ModelSize(val value: String, val displayName: String) {
    STANDARD("standard", "Standard"),
    LITE("lite", "Lite"),
    FEATHER("feather", "Feather"),
    NANO("nano", "Nano"),
    CUSTOM("custom", "Custom");

    companion object {
        fun fromString(value: String?): String {
            if (value == null) return "Unknown"
            return values().find { it.value.lowercase() == value.lowercase() }?.displayName ?: value.replaceFirstChar { it.uppercase() }
        }
    }
}

enum class TonesSort(val value: String, val displayName: String) {
    BEST_MATCH("best-match", "Best Match"),
    NEWEST("newest", "Newest"),
    OLDEST("oldest", "Oldest"),
    TRENDING("trending", "Trending"),
    DOWNLOADS_ALL_TIME("downloads-all-time", "Most Downloads");

    companion object {
        fun fromString(value: String?): String {
            if (value == null) return "Best Match"
            return values().find { it.value.lowercase() == value.lowercase() }?.displayName ?: "Best Match"
        }
    }
}
