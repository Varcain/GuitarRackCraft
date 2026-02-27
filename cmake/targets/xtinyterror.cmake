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
# cmake/targets/xtinyterror.cmake — Build XTinyTerror LV2 plugin
# =============================================================================

set(_xtt_src       "${THIRD_PARTY}/XTinyTerror")
set(_xtt_plugin    "${_xtt_src}/XTinyTerror")
set(_xtt_assets    "${ASSETS_DIR}/XTinyTerror.lv2")

# ─── Phase 1: Copy TTL ──────────────────────────────────────────────────
file(MAKE_DIRECTORY "${_xtt_assets}")
if(EXISTS "${_xtt_plugin}/plugin/manifest.ttl")
    configure_file("${_xtt_plugin}/plugin/manifest.ttl" "${_xtt_assets}/manifest.ttl" COPYONLY)
endif()
if(EXISTS "${_xtt_plugin}/plugin/XTinyTerror.ttl")
    configure_file("${_xtt_plugin}/plugin/XTinyTerror.ttl" "${_xtt_assets}/XTinyTerror.ttl" COPYONLY)
endif()

# ─── Phase 2 & 3: Build & Sync ──────────────────────────────────────────
brummer_add_plugin(
    NAME xtinyterror
    OUTPUT_NAME XTinyTerror
    SOURCE_DIR "${_xtt_src}"
    DSP_SOURCE "${_xtt_plugin}/plugin/XTinyTerror.cpp"
    UI_SOURCE  "${_xtt_plugin}/gui/XTinyTerror_x11ui.c"
    ASSETS_DIR "${_xtt_assets}"
    PNG_DIR    "${_xtt_plugin}/gui"
    DSP_INCLUDES
        "${_xtt_plugin}/plugin"
        "${_xtt_plugin}/dsp"
    UI_INCLUDES
        "${_xtt_plugin}/plugin"
        "${_xtt_plugin}/gui"
    UI_DEFINITIONS
        -DUSE_ATOM
)
