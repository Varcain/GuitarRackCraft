/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.varcain.guitarrackcraft.ui.vst

import androidx.navigation.NavController
import androidx.navigation.NavGraphBuilder
import androidx.navigation.compose.composable

const val VST_MANAGER_ROUTE = "vst_manager"

fun NavGraphBuilder.vstManagerRoute(navController: NavController) {
    composable(VST_MANAGER_ROUTE) {
        VstManagerScreen(onNavigateBack = { navController.popBackStack() })
    }
}
