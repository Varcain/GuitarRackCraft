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
# cmake/targets/metadata.cmake — Generate plugin metadata + shared modgui resources
#
# Must run last — scans all plugin assets for available binaries.
# =============================================================================

set(_stamps_dir "${CMAKE_BINARY_DIR}/stamps")
file(MAKE_DIRECTORY "${_stamps_dir}")

# ─── All plugin targets that metadata depends on ─────────────────────────────
set(_all_plugin_deps
    gx_plugins_done
    trunk_plugins_done
    plugin_uis_done
    nam_done
    aidax_done
    aidax_full_done
    neuralrack_done
    impulseloader_done
    xdarkterror_done
    xtinyterror_done
    collisiondrive_done
    metaltone_done
    gxcabsim_done
    modamptk_done
    fatfrog_done
)

# ─── Plugin metadata JSON (stamp-based) ─────────────────────────────────────
set(_metadata_generator_script "${PROJECT_ROOT}/cmake/modules/GeneratePluginMetadata.cmake")
set(_metadata_stamp "${_stamps_dir}/metadata_json.stamp")

add_custom_command(
    OUTPUT "${_metadata_stamp}"
    COMMAND ${CMAKE_COMMAND}
        -DPROJECT_ROOT=${PROJECT_ROOT}
        -DASSETS_DIR=${ASSETS_DIR}
        -P "${_metadata_generator_script}"
    COMMAND ${CMAKE_COMMAND} -E touch "${_metadata_stamp}"
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    DEPENDS ${_all_plugin_deps}
    COMMENT "Generating plugin_metadata.json"
)
add_custom_target(metadata_json DEPENDS "${_metadata_stamp}")

# ─── Shared modgui resources (stamp-based) ───────────────────────────────────
set(_modgui_resources_script "${PROJECT_ROOT}/cmake/modules/CopyModguiResources.cmake")
set(_modgui_stamp "${_stamps_dir}/modgui_resources.stamp")

add_custom_command(
    OUTPUT "${_modgui_stamp}"
    COMMAND ${CMAKE_COMMAND}
        -DPROJECT_ROOT=${PROJECT_ROOT}
        -DTHIRD_PARTY=${THIRD_PARTY}
        -DASSETS_RESOURCES=${ASSETS_DIR}/modgui_shared_resources/resources
        -P "${_modgui_resources_script}"
    COMMAND ${CMAKE_COMMAND} -E touch "${_modgui_stamp}"
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    DEPENDS
        gx_plugins_done
        trunk_plugins_done
    COMMENT "Building shared modgui resources"
)
add_custom_target(modgui_resources DEPENDS "${_modgui_stamp}")

# ─── Strip redundant .so from assets (stamp-based) ──────────────────────────
set(_strip_stamp "${_stamps_dir}/strip_asset_binaries.stamp")

add_custom_command(
    OUTPUT "${_strip_stamp}"
    COMMAND find "${ASSETS_DIR}" -name "*.so" -type f -delete
    COMMAND ${CMAKE_COMMAND} -E touch "${_strip_stamp}"
    DEPENDS metadata_json
    COMMENT "Stripping redundant plugin .so from assets"
)
add_custom_target(strip_asset_binaries DEPENDS "${_strip_stamp}")

# ─── Aggregate metadata target ───────────────────────────────────────────────
add_custom_target(metadata_done
    DEPENDS metadata_json modgui_resources strip_asset_binaries
    COMMENT "Metadata + modgui complete"
)
