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

#include <string>

#if defined(HAVE_LV2) && HAVE_LV2 == 1
#include <lilv/lilv.h>
#endif

namespace guitarrackcraft {

#if defined(HAVE_LV2) && HAVE_LV2 == 1

/** Resolve the X11 UI binary path from a lilv UI entry.
 *  Tries the parsed file URI first, then falls back to bundle root + filename.
 *  Returns empty string if binary not found on disk. */
std::string resolveX11UIBinaryPath(const LilvUI* ui, const LilvPlugin* plugin, LilvWorld* world);

/** Discover modgui metadata (iconTemplate + basePath) from a plugin's bundle.
 *  Returns true if modgui was found and info fields were populated. */
struct PluginInfo;
bool discoverModguiMetadata(const LilvPlugin* plugin, PluginInfo& info);

#endif

} // namespace guitarrackcraft
