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

#pragma once

namespace guitarrackcraft {

/** True while createPluginUI is running (plugin UI executor thread).
 *  Used to defer X11 signalDetach: if surfaceDestroyed runs while the plugin is blocked in
 *  XGetWindowAttributes, we must not close the X client fd yet, or Xlib's default XIOErrorHandler
 *  will call exit(1) and kill the process (~AAudioLoader then runs during unload). */
bool isCreatingPluginUI();
void setCreatingPluginUI(bool value);

/** Check if plugin UI creation is in progress for a specific display number.
 *  Used by X11NativeDisplay to defer closing the X11 connection during plugin init. */
bool isCreatingPluginUIForDisplay(int displayNumber);

}  // namespace guitarrackcraft
