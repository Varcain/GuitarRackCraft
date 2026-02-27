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
# cmake/targets/xdarkterror.cmake — Build XDarkTerror LV2 plugin
# =============================================================================

set(_xdt_src       "${THIRD_PARTY}/XDarkTerror")
set(_xdt_plugin    "${_xdt_src}/XDarkTerror")
set(_xdt_assets    "${ASSETS_DIR}/XDarkTerror.lv2")

# ─── Phase 1: Copy TTL ──────────────────────────────────────────────────
file(MAKE_DIRECTORY "${_xdt_assets}")
if(EXISTS "${_xdt_plugin}/plugin/manifest.ttl")
    configure_file("${_xdt_plugin}/plugin/manifest.ttl" "${_xdt_assets}/manifest.ttl" COPYONLY)
endif()
if(EXISTS "${_xdt_plugin}/plugin/XDarkTerror.ttl")
    configure_file("${_xdt_plugin}/plugin/XDarkTerror.ttl" "${_xdt_assets}/XDarkTerror.ttl" COPYONLY)
endif()

# ─── Phase 2 & 3: Build & Sync ──────────────────────────────────────────
brummer_add_plugin(
    NAME xdarkterror
    OUTPUT_NAME XDarkTerror
    SOURCE_DIR "${_xdt_src}"
    DSP_SOURCE "${_xdt_plugin}/plugin/XDarkTerror.cpp"
    UI_SOURCE  "${_xdt_plugin}/gui/XDarkTerror_x11ui.c"
    ASSETS_DIR "${_xdt_assets}"
    PNG_DIR    "${_xdt_plugin}/gui"
    DSP_INCLUDES
        "${_xdt_plugin}/plugin"
        "${_xdt_plugin}/dsp"
        "${_xdt_plugin}/dsp/zita-resampler-1.1.0"
    UI_INCLUDES
        "${_xdt_plugin}/plugin"
        "${_xdt_plugin}/gui"
    UI_DEFINITIONS
        -DUSE_ATOM
)
