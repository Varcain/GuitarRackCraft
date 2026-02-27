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
# cmake/targets/fatfrog.cmake — Build FatFrog LV2 plugin
# =============================================================================

set(_ff_src       "${THIRD_PARTY}/FatFrog.lv2")
set(_ff_plugin    "${_ff_src}/FatFrog")
set(_ff_assets    "${ASSETS_DIR}/FatFrog.lv2")

# ─── Phase 1: Copy TTL ──────────────────────────────────────────────────
file(MAKE_DIRECTORY "${_ff_assets}")
if(EXISTS "${_ff_plugin}/plugin/manifest.ttl")
    configure_file("${_ff_plugin}/plugin/manifest.ttl" "${_ff_assets}/manifest.ttl" COPYONLY)
endif()
if(EXISTS "${_ff_plugin}/plugin/FatFrog.ttl")
    configure_file("${_ff_plugin}/plugin/FatFrog.ttl" "${_ff_assets}/FatFrog.ttl" COPYONLY)
endif()

# ─── Phase 2 & 3: Build & Sync ──────────────────────────────────────────
brummer_add_plugin(
    NAME fatfrog
    OUTPUT_NAME FatFrog
    SOURCE_DIR "${_ff_src}"
    DSP_SOURCE "${_ff_plugin}/plugin/FatFrog.cpp"
    UI_SOURCE  "${_ff_plugin}/gui/FatFrog_x11ui.c"
    ASSETS_DIR "${_ff_assets}"
    PNG_DIR    "${_ff_plugin}/gui"
    DSP_INCLUDES
        "${_ff_plugin}/plugin"
        "${_ff_plugin}/dsp"
    UI_INCLUDES
        "${_ff_plugin}/plugin"
        "${_ff_plugin}/gui"
)
