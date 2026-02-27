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

import android.util.Log
import com.google.gson.Gson
import com.google.gson.reflect.TypeToken
import com.varcain.guitarrackcraft.BuildConfig
import okhttp3.*
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.RequestBody.Companion.toRequestBody
import java.io.IOException

class ApiException(val code: Int, val errorBody: String?) : Exception("API Error $code: $errorBody")

class Tone3000Api(private val tokenManager: TokenManager) {
    private val gson = Gson()
    private val baseUrl = "https://www.tone3000.com/api/v1"
    private val tag = "Tone3000Api"
    private val userAgent = "GuitarRackCraft/${BuildConfig.VERSION_NAME}"
    private val appId = "GuitarRackCraft"

    private val client = OkHttpClient.Builder()
        .addInterceptor { chain ->
            val requestBuilder = chain.request().newBuilder()
            requestBuilder.header("User-Agent", userAgent)
            requestBuilder.header("X-App-Id", appId)
            
            tokenManager.accessToken?.let { token ->
                if (token.isNotEmpty()) {
                    requestBuilder.header("Authorization", "Bearer $token")
                }
            }
            chain.proceed(requestBuilder.build())
        }
        .authenticator(object : Authenticator {
            override fun authenticate(route: Route?, response: Response): Request? {
                if (response.count401() > 2) {
                    Log.w(tag, "Too many 401s, giving up")
                    return null
                }

                val refreshToken = tokenManager.refreshToken ?: return null
                val accessToken = tokenManager.accessToken ?: return null

                Log.d(tag, "Attempting token refresh")
                synchronized(this) {
                    if (tokenManager.accessToken != accessToken) {
                        return response.request.newBuilder()
                            .header("Authorization", "Bearer ${tokenManager.accessToken}")
                            .build()
                    }

                    val newSession = try {
                        refreshSession(refreshToken, accessToken)
                    } catch (e: Exception) {
                        null
                    }
                    if (newSession != null) {
                        Log.i(tag, "Token refreshed successfully")
                        tokenManager.accessToken = newSession.access_token
                        tokenManager.refreshToken = newSession.refresh_token
                        return response.request.newBuilder()
                            .header("Authorization", "Bearer ${newSession.access_token}")
                            .build()
                    }
                }
                Log.e(tag, "Token refresh failed")
                return null
            }
        })
        .build()

    private fun Response.count401(): Int {
        var result = 1
        var r = priorResponse
        while (r != null) {
            result++
            r = r.priorResponse
        }
        return result
    }

    @Throws(ApiException::class, IOException::class)
    fun exchangeApiKey(apiKey: String): Session? {
        val json = gson.toJson(AuthRequest(apiKey))
        val body = json.toRequestBody("application/json".toMediaType())
        val request = Request.Builder()
            .url("$baseUrl/auth/session")
            .header("User-Agent", userAgent)
            .header("X-App-Id", appId)
            .post(body)
            .build()

        return try {
            OkHttpClient().newCall(request).execute().use { response ->
                val responseBody = response.body?.string()
                if (!response.isSuccessful) {
                    Log.e(tag, "exchangeApiKey failed: ${response.code} - $responseBody")
                    throw ApiException(response.code, responseBody)
                }
                gson.fromJson(responseBody, Session::class.java)
            }
        } catch (e: Exception) {
            if (e is ApiException) throw e
            Log.e(tag, "exchangeApiKey exception", e)
            throw e
        }
    }

    @Throws(ApiException::class, IOException::class)
    private fun refreshSession(refreshToken: String, accessToken: String): Session? {
        val json = gson.toJson(RefreshRequest(refreshToken, accessToken))
        val body = json.toRequestBody("application/json".toMediaType())
        val request = Request.Builder()
            .url("$baseUrl/auth/session/refresh")
            .header("User-Agent", userAgent)
            .header("X-App-Id", appId)
            .post(body)
            .build()

        return try {
            OkHttpClient().newCall(request).execute().use { response ->
                val responseBody = response.body?.string()
                if (!response.isSuccessful) {
                    Log.e(tag, "refreshSession failed: ${response.code} - $responseBody")
                    throw ApiException(response.code, responseBody)
                }
                gson.fromJson(responseBody, Session::class.java)
            }
        } catch (e: Exception) {
            if (e is ApiException) throw e
            Log.e(tag, "refreshSession exception", e)
            throw e
        }
    }

    @Throws(ApiException::class, IOException::class)
    fun getUser(): User? {
        val request = Request.Builder().url("$baseUrl/user").build()
        return try {
            client.newCall(request).execute().use { response ->
                val responseBody = response.body?.string()
                if (!response.isSuccessful) {
                    Log.e(tag, "getUser failed: ${response.code} - $responseBody")
                    throw ApiException(response.code, responseBody)
                }
                val type = object : TypeToken<DataWrapper<User>>() {}.type
                val wrapper: DataWrapper<User> = gson.fromJson(responseBody, type)
                wrapper.data
            }
        } catch (e: Exception) {
            if (e is ApiException) throw e
            Log.e(tag, "getUser exception", e)
            throw e
        }
    }

    @Throws(ApiException::class, IOException::class)
    fun searchTones(
        query: String = "",
        page: Int = 1,
        pageSize: Int = 10,
        gear: String? = null,
        sizes: String? = null,
        calibrated: Boolean? = null,
        sort: String? = null
    ): PaginatedResponse<List<Tone>>? {
        val urlBuilder = "$baseUrl/tones/search".toHttpUrlOrNull()?.newBuilder() ?: return null
        if (query.isNotEmpty()) urlBuilder.addQueryParameter("query", query)
        urlBuilder.addQueryParameter("page", page.toString())
        urlBuilder.addQueryParameter("page_size", pageSize.toString())
        if (!gear.isNullOrEmpty()) urlBuilder.addQueryParameter("gear", gear)
        if (!sizes.isNullOrEmpty()) urlBuilder.addQueryParameter("sizes", sizes)
        if (calibrated != null) urlBuilder.addQueryParameter("calibrated", calibrated.toString())
        if (!sort.isNullOrEmpty()) urlBuilder.addQueryParameter("sort", sort)
        
        val request = Request.Builder().url(urlBuilder.build()).build()

        Log.d(tag, "Searching tones: ${request.url}")
        return try {
            client.newCall(request).execute().use { response ->
                val responseBody = response.body?.string()
                Log.d(tag, "searchTones response: $responseBody")
                if (!response.isSuccessful) {
                    Log.e(tag, "searchTones failed: ${response.code} - $responseBody")
                    throw ApiException(response.code, responseBody)
                }
                try {
                    val type = object : TypeToken<DataWrapper<PaginatedResponse<List<Tone>>>>() {}.type
                    val wrapper: DataWrapper<PaginatedResponse<List<Tone>>> = gson.fromJson(responseBody, type)
                    wrapper.data
                } catch (e: Exception) {
                    val type = object : TypeToken<PaginatedResponse<List<Tone>>>() {}.type
                    gson.fromJson(responseBody, type)
                }
            }
        } catch (e: Exception) {
            if (e is ApiException) throw e
            Log.e(tag, "searchTones exception", e)
            throw e
        }
    }

    @Throws(ApiException::class, IOException::class)
    fun getModels(toneId: String, pageSize: Int = 10): List<Model>? {
        val allModels = mutableListOf<Model>()
        var currentPage = 1
        var totalPages = 1

        do {
            val url = "$baseUrl/models?tone_id=$toneId&page=$currentPage&page_size=$pageSize"
            val request = Request.Builder().url(url).build()
            
            try {
                client.newCall(request).execute().use { response ->
                    val responseBody = response.body?.string()
                    if (!response.isSuccessful) {
                        Log.e(tag, "getModels failed (page $currentPage): ${response.code} - $responseBody")
                        throw ApiException(response.code, responseBody)
                    }
                    val type = object : TypeToken<PaginatedResponse<List<Model>>>() {}.type
                    val paginated: PaginatedResponse<List<Model>> = gson.fromJson(responseBody, type)
                    allModels.addAll(paginated.data)
                    totalPages = paginated.total_pages
                    currentPage++
                }
            } catch (e: Exception) {
                if (e is ApiException) throw e
                Log.e(tag, "getModels exception (page $currentPage)", e)
                throw e
            }
        } while (currentPage <= totalPages)

        return allModels
    }

    @Throws(ApiException::class, IOException::class)
    fun getToneFromUrl(toneUrl: String): Tone? {
        val url = if (toneUrl.startsWith("http")) {
            toneUrl
        } else {
            val cleanToneUrl = if (toneUrl.startsWith("/")) toneUrl else "/$toneUrl"
            "$baseUrl$cleanToneUrl"
        }
        Log.d(tag, "getToneFromUrl: $url")
        val request = Request.Builder().url(url).build()
        
        return try {
            client.newCall(request).execute().use { response ->
                val responseBody = response.body?.string()
                if (!response.isSuccessful) {
                    Log.e(tag, "getToneFromUrl failed: ${response.code} - $responseBody")
                    throw ApiException(response.code, responseBody)
                }
                try {
                    val type = object : TypeToken<DataWrapper<Tone>>() {}.type
                    val wrapper: DataWrapper<Tone> = gson.fromJson(responseBody, type)
                    wrapper.data
                } catch (e: Exception) {
                    gson.fromJson(responseBody, Tone::class.java)
                }
            }
        } catch (e: Exception) {
            if (e is ApiException) throw e
            Log.e(tag, "getToneFromUrl exception", e)
            throw e
        }
    }

    fun downloadFile(url: String, destFile: java.io.File): Boolean {
        val request = Request.Builder().url(url).build()
        return try {
            client.newCall(request).execute().use { response ->
                if (!response.isSuccessful) {
                    Log.e(tag, "downloadFile failed: ${response.code}")
                    return false
                }
                response.body?.byteStream()?.use { input ->
                    destFile.outputStream().use { output ->
                        input.copyTo(output)
                    }
                }
                true
            }
        } catch (e: Exception) {
            Log.e(tag, "downloadFile exception", e)
            false
        }
    }
}
