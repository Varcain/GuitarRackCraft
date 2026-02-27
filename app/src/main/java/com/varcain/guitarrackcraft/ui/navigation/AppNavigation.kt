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

package com.varcain.guitarrackcraft.ui.navigation

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.navigation.NavHostController
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import androidx.lifecycle.viewmodel.compose.viewModel
import com.varcain.guitarrackcraft.ui.browser.PluginBrowserScreen
import com.varcain.guitarrackcraft.ui.modgui.ModguiScreen
import com.varcain.guitarrackcraft.ui.rack.RackScreen
import com.varcain.guitarrackcraft.ui.rack.RackViewModel
import com.varcain.guitarrackcraft.ui.recordings.RecordingsScreen
import com.varcain.guitarrackcraft.ui.settings.AudioSettingsScreen
import com.varcain.guitarrackcraft.ui.tone3000.Tone
import com.varcain.guitarrackcraft.ui.tone3000.Tone3000Screen
import com.varcain.guitarrackcraft.ui.tone3000.ToneDetailScreen

sealed class Screen(val route: String) {
    object Rack : Screen("rack")
    object Browser : Screen("browser?replaceIndex={replaceIndex}") {
        fun route(replaceIndex: Int = -1) = "browser?replaceIndex=$replaceIndex"
    }
    object Modgui : Screen("modgui/{pluginIndex}") {
        fun route(pluginIndex: Int) = "modgui/$pluginIndex"
    }
    object Settings : Screen("settings")
    object Recordings : Screen("recordings")
    object Tone3000 : Screen("tone3000?tag={tag}&gear={gear}&platform={platform}&sourcePlugin={sourcePlugin}&sourceSlot={sourceSlot}") {
        fun route(tag: String? = null, gear: String? = null, platform: String? = null, sourcePluginIndex: Int = -1, sourceSlot: String? = null): String {
            val tagPart = tag?.let { "tag=$it" } ?: ""
            val gearPart = gear?.let { "gear=$it" } ?: ""
            val platformPart = platform?.let { "platform=$it" } ?: ""
            val sourcePart = if (sourcePluginIndex >= 0) "sourcePlugin=$sourcePluginIndex" else ""
            val slotPart = sourceSlot?.let { "sourceSlot=$it" } ?: ""
            val query = listOf(tagPart, gearPart, platformPart, sourcePart, slotPart).filter { it.isNotEmpty() }.joinToString("&")
            return if (query.isNotEmpty()) "tone3000?$query" else "tone3000"
        }
    }
    object ToneDetail : Screen("tone_detail/{toneId}?sourcePlugin={sourcePlugin}&sourceSlot={sourceSlot}") {
        fun route(toneId: String, sourcePluginIndex: Int = -1, sourceSlot: String? = null): String {
            val parts = mutableListOf<String>()
            if (sourcePluginIndex >= 0) parts.add("sourcePlugin=$sourcePluginIndex")
            if (sourceSlot != null) parts.add("sourceSlot=$sourceSlot")
            val query = parts.joinToString("&")
            return if (query.isNotEmpty()) "tone_detail/$toneId?$query" else "tone_detail/$toneId"
        }
    }
}

@Composable
fun AppNavigation(
    navController: NavHostController = rememberNavController()
) {
    val backStackEntry by navController.currentBackStackEntryAsState()
    val currentRoute = backStackEntry?.destination?.route
    val rackViewModel: RackViewModel = viewModel()

    Box(modifier = Modifier.fillMaxSize()) {
        // RackScreen is ALWAYS composed — kept alive so X11/modgui UIs don't re-render.
        val isRackVisible = currentRoute == null || currentRoute == Screen.Rack.route
        RackScreen(
            isVisible = isRackVisible,
            onNavigateToBrowser = { navController.navigate(Screen.Browser.route(-1)) },
            onNavigateToSettings = { navController.navigate(Screen.Settings.route) },
            onNavigateToRecordings = { navController.navigate(Screen.Recordings.route) },
            onNavigateToTone3000 = { tag, gear, platform, sourcePluginIndex, sourceSlot ->
                navController.navigate(Screen.Tone3000.route(tag, gear, platform, sourcePluginIndex, sourceSlot))
            },
            onOpenModgui = { pluginIndex ->
                navController.navigate(Screen.Modgui.route(pluginIndex))
            },
            onReplacePlugin = { replaceIndex ->
                navController.navigate(Screen.Browser.route(replaceIndex))
            },
            viewModel = rackViewModel
        )

        // NavHost is always composed (navController needs its graph).
        // The rack route is an empty placeholder; the real RackScreen lives above.
        // Other screens overlay on top of the rack when navigated to.
        NavHost(
            navController = navController,
            startDestination = Screen.Rack.route,
            modifier = Modifier.fillMaxSize()
        ) {
            composable(Screen.Rack.route) {
                // Empty — RackScreen is always composed above
            }
            composable(
                route = Screen.Modgui.route,
                arguments = listOf(navArgument("pluginIndex") { type = NavType.IntType })
            ) { entry ->
                val pluginIndex = entry.arguments?.getInt("pluginIndex") ?: 0
                ModguiScreen(
                    pluginIndex = pluginIndex,
                    onNavigateBack = { navController.popBackStack() }
                )
            }
            composable(
                route = Screen.Browser.route,
                arguments = listOf(navArgument("replaceIndex") {
                    type = NavType.IntType
                    defaultValue = -1
                })
            ) { entry ->
                val replaceIndex = entry.arguments?.getInt("replaceIndex") ?: -1
                PluginBrowserScreen(
                    replaceIndex = replaceIndex,
                    onNavigateBack = { navController.popBackStack() }
                )
            }
            composable(Screen.Settings.route) {
                AudioSettingsScreen(
                    viewModel = rackViewModel,
                    onNavigateBack = { navController.popBackStack() }
                )
            }
            composable(Screen.Recordings.route) {
                RecordingsScreen(
                    onNavigateBack = { navController.popBackStack() },
                    onPlayRecording = { path ->
                        navController.popBackStack()
                        val fileName = java.io.File(path).name
                        // Raw recordings should be processed through effects;
                        // processed recordings already have effects baked in.
                        val isRaw = fileName.startsWith("Raw_")
                        rackViewModel.setWavProcessEffects(isRaw)
                        rackViewModel.loadWav(path, fileName)
                    }
                )
            }
            composable(
                route = Screen.Tone3000.route,
                arguments = listOf(
                    navArgument("tag") { type = NavType.StringType; nullable = true; defaultValue = null },
                    navArgument("gear") { type = NavType.StringType; nullable = true; defaultValue = null },
                    navArgument("platform") { type = NavType.StringType; nullable = true; defaultValue = null },
                    navArgument("sourcePlugin") { type = NavType.IntType; defaultValue = -1 },
                    navArgument("sourceSlot") { type = NavType.StringType; nullable = true; defaultValue = null }
                )
            ) { entry ->
                val tag = entry.arguments?.getString("tag")
                val gear = entry.arguments?.getString("gear")
                val platform = entry.arguments?.getString("platform")
                val sourcePluginIndex = entry.arguments?.getInt("sourcePlugin") ?: -1
                val sourceSlot = entry.arguments?.getString("sourceSlot")
                Tone3000Screen(
                    onNavigateBack = { navController.popBackStack() },
                    onNavigateToDetail = { tone ->
                        // Store the selected tone in the back stack entry's saved state
                        navController.currentBackStackEntry?.savedStateHandle?.set("selected_tone", tone)
                        navController.navigate(Screen.ToneDetail.route(tone.id, sourcePluginIndex, sourceSlot))
                    },
                    initialTag = tag,
                    initialGear = gear,
                    initialPlatform = platform,
                    sourcePluginIndex = sourcePluginIndex,
                    sourceSlot = sourceSlot
                )
            }
            composable(
                route = Screen.ToneDetail.route,
                arguments = listOf(
                    navArgument("toneId") { type = NavType.StringType },
                    navArgument("sourcePlugin") { type = NavType.IntType; defaultValue = -1 },
                    navArgument("sourceSlot") { type = NavType.StringType; nullable = true; defaultValue = null }
                )
            ) { entry ->
                val toneId = entry.arguments?.getString("toneId") ?: ""
                val sourcePluginIndex = entry.arguments?.getInt("sourcePlugin") ?: -1
                val sourceSlot = entry.arguments?.getString("sourceSlot")
                val selectedTone = navController.previousBackStackEntry?.savedStateHandle?.get<Tone>("selected_tone")

                ToneDetailScreen(
                    toneId = toneId,
                    initialTone = selectedTone,
                    onNavigateBack = { navController.popBackStack() },
                    sourcePluginIndex = sourcePluginIndex,
                    sourceSlot = sourceSlot
                )
            }
        }
    }
}
