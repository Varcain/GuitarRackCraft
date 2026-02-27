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
# cmake/targets/collisiondrive.cmake — Build CollisionDrive LV2 plugin
# =============================================================================

set(_cd_src       "${THIRD_PARTY}/CollisionDrive")
set(_cd_plugin    "${_cd_src}/CollisionDrive")
set(_cd_assets    "${ASSETS_DIR}/CollisionDrive.lv2")

# ─── Phase 1: Copy TTL ──────────────────────────────────────────────────
file(MAKE_DIRECTORY "${_cd_assets}")
if(EXISTS "${_cd_plugin}/manifest.ttl")
    configure_file("${_cd_plugin}/manifest.ttl" "${_cd_assets}/manifest.ttl" COPYONLY)
endif()
if(EXISTS "${_cd_plugin}/CollisionDrive.ttl")
    configure_file("${_cd_plugin}/CollisionDrive.ttl" "${_cd_assets}/CollisionDrive.ttl" COPYONLY)
endif()

# ─── Phase 2 & 3: Build & Sync ──────────────────────────────────────────
brummer_add_plugin(
    NAME collisiondrive
    OUTPUT_NAME CollisionDrive
    SOURCE_DIR "${_cd_src}"
    DSP_SOURCE "${_cd_plugin}/CollisionDrive.cpp"
    UI_SOURCE  "${_cd_plugin}/CollisionDrive.c"
    ASSETS_DIR "${_cd_assets}"
    PNG_DIR    "${_cd_src}/resources"
    DSP_INCLUDES
        "${_cd_plugin}"
        "${_cd_plugin}/dsp"
        "${_cd_plugin}/dsp/zita-resampler-1.1.0"
    UI_INCLUDES
        "${_cd_plugin}"
)
