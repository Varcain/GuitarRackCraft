/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.varcain.guitarrackcraft.ui.vst

/** playstore-flavor stub. No VSTs in this flavor → no inline editor → the
 *  rack chrome never shows the Keyboard button. */
object VstKeyboardAction {
    fun showKeyboard(pluginIndex: Int) { /* no-op */ }
}
