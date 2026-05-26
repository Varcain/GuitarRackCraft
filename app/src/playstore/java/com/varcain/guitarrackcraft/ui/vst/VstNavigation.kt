/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.varcain.guitarrackcraft.ui.vst

import androidx.navigation.NavController
import androidx.navigation.NavGraphBuilder

/**
 * playstore-flavor stub. VST hosting requires :vsthost_lib (full flavor only),
 * so this route is a no-op here. The "Manage VST" overflow menu entry is also
 * hidden via BuildConfig.HAS_VST_HOST in RackScreen.kt, so this never triggers.
 */
const val VST_MANAGER_ROUTE = "vst_manager"

fun NavGraphBuilder.vstManagerRoute(navController: NavController) {
    // intentionally empty — playstore flavor has no VST hosting
}
