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
# cmake/targets/metaltone.cmake — Build MetalTone LV2 plugin
# =============================================================================

set(_mt_src       "${THIRD_PARTY}/MetalTone")
set(_mt_plugin    "${_mt_src}/MetalTone")
set(_mt_assets    "${ASSETS_DIR}/MetalTone.lv2")

# ─── Phase 1: Copy TTL ──────────────────────────────────────────────────
file(MAKE_DIRECTORY "${_mt_assets}")
if(EXISTS "${_mt_plugin}/manifest.ttl")
    configure_file("${_mt_plugin}/manifest.ttl" "${_mt_assets}/manifest.ttl" COPYONLY)
endif()
if(EXISTS "${_mt_plugin}/MetalTone.ttl")
    configure_file("${_mt_plugin}/MetalTone.ttl" "${_mt_assets}/MetalTone.ttl" COPYONLY)
endif()

# ─── Phase 2 & 3: Build & Sync ──────────────────────────────────────────
brummer_add_plugin(
    NAME metaltone
    OUTPUT_NAME MetalTone
    SOURCE_DIR "${_mt_src}"
    DSP_SOURCE "${_mt_plugin}/MetalTone.cpp"
    UI_SOURCE  "${_mt_plugin}/MetalTone.c"
    ASSETS_DIR "${_mt_assets}"
    PNG_DIR    "${_mt_src}/resources"
    DSP_INCLUDES
        "${_mt_plugin}"
        "${_mt_plugin}/zita-resampler-1.1.0"
    UI_INCLUDES
        "${_mt_plugin}"
)
