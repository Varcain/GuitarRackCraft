# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# This file is part of Guitar RackCraft.
#
# Guitar RackCraft is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Guitar RackCraft is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.

# =============================================================================
# cmake/modules/CopyModguiResources.cmake — Refactored from build_modgui.sh
# =============================================================================

# Inputs:
#   PROJECT_ROOT
#   THIRD_PARTY
#   ASSETS_RESOURCES

if(NOT PROJECT_ROOT OR NOT THIRD_PARTY OR NOT ASSETS_RESOURCES)
    message(FATAL_ERROR "PROJECT_ROOT, THIRD_PARTY, and ASSETS_RESOURCES must be set")
endif()

set(GX_PLUGINS_DIR "${THIRD_PARTY}/GxPlugins.lv2.Android")
set(TRUNK_LV2_DIR "${THIRD_PARTY}/guitarix/trunk/src/LV2")

message(STATUS "Setting up MOD shared resources for modgui")

if(NOT IS_DIRECTORY "${GX_PLUGINS_DIR}")
    message(FATAL_ERROR "GxPlugins.lv2.Android not found at ${GX_PLUGINS_DIR}")
endif()

file(MAKE_DIRECTORY "${ASSETS_RESOURCES}")

set(MODGUI_SUBS pedals knobs utils switches heads combos)

macro(copy_modgui_resources plugin_dir modgui_subdir)
    foreach(sub IN LISTS MODGUI_SUBS)
        if(IS_DIRECTORY "${plugin_dir}/${modgui_subdir}/${sub}")
            file(MAKE_DIRECTORY "${ASSETS_RESOURCES}/${sub}")
            file(GLOB _files "${plugin_dir}/${modgui_subdir}/${sub}/*")
            if(_files)
                file(COPY ${_files} DESTINATION "${ASSETS_RESOURCES}/${sub}")
            endif()
        endif()
    endforeach()
endmacro()

# Collect shared resources from GxPlugins.lv2.Android
file(GLOB _gx_plugins LIST_DIRECTORIES true "${GX_PLUGINS_DIR}/*.lv2")
foreach(plugin_dir IN LISTS _gx_plugins)
    if(IS_DIRECTORY "${plugin_dir}/MOD/modgui")
        copy_modgui_resources("${plugin_dir}" "MOD/modgui")
    endif()
endforeach()

# Collect shared resources from trunk plugins
if(IS_DIRECTORY "${TRUNK_LV2_DIR}")
    file(GLOB _trunk_plugins LIST_DIRECTORIES true "${TRUNK_LV2_DIR}/*.lv2")
    foreach(plugin_dir IN LISTS _trunk_plugins)
        if(IS_DIRECTORY "${plugin_dir}/modgui")
            copy_modgui_resources("${plugin_dir}" "modgui")
        endif()
    endforeach()
endif()

# Add knobs/boxy/boxy.png if missing
if(NOT EXISTS "${ASSETS_RESOURCES}/knobs/boxy/boxy.png")
    file(GLOB_RECURSE _boxy_png "${GX_PLUGINS_DIR}/*/MOD/modgui/knobs/boxy/boxy.png")
    if(_boxy_png)
        list(GET _boxy_png 0 _first_boxy)
        file(MAKE_DIRECTORY "${ASSETS_RESOURCES}/knobs/boxy")
        file(COPY "${_first_boxy}" DESTINATION "${ASSETS_RESOURCES}/knobs/boxy")
    endif()
endif()

message(STATUS "MOD shared resources installed at ${ASSETS_RESOURCES}")
