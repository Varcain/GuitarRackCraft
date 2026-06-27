/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.varcain.guitarrackcraft.ui.vst

import androidx.compose.runtime.Composable

/** playstore-flavor stub. VST hosting isn't in this flavor, so the
 *  inline editor is a no-op (and rack code never calls it because there
 *  are no VST2-format plugins in the registry). */
@Composable
fun VstInlineEditor(
    pluginIndex: Int,
    isFullscreen: Boolean = false,
    onPluginSizeKnown: (width: Int, height: Int) -> Unit = { _, _ -> },
    onFilePickerRequested: (
        sequence: Int,
        title: String,
        filterPatterns: String,
        initialDir: String,
        copyDirLinux: String,
        copyDirWindows: String,
    ) -> Boolean = { _, _, _, _, _, _ -> false },
) { /* no-op */ }
