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
    doubletracker_done
)

# The three steps below read every staged bundle - a set only known once the
# plugin targets have run - so they run on every build (well under a second
# together) instead of behind a stamp that nothing would invalidate. They
# rewrite only what changed, so Gradle still sees an up-to-date asset tree.

# ─── Plugin metadata JSON ───────────────────────────────────────────────────
add_custom_target(metadata_json
    COMMAND ${CMAKE_COMMAND}
        -DPROJECT_ROOT=${PROJECT_ROOT}
        -DASSETS_DIR=${ASSETS_DIR}
        -P "${PROJECT_ROOT}/cmake/modules/GeneratePluginMetadata.cmake"
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    COMMENT "Generating plugin_metadata.json"
)
add_dependencies(metadata_json ${_all_plugin_deps})

# ─── Shared modgui resources ─────────────────────────────────────────────────
add_custom_target(modgui_resources
    COMMAND ${CMAKE_COMMAND}
        -DPROJECT_ROOT=${PROJECT_ROOT}
        -DTHIRD_PARTY=${THIRD_PARTY}
        -DASSETS_RESOURCES=${ASSETS_DIR}/modgui_shared_resources/resources
        -P "${PROJECT_ROOT}/cmake/modules/CopyModguiResources.cmake"
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    COMMENT "Building shared modgui resources"
)
add_dependencies(modgui_resources gx_plugins_done trunk_plugins_done)

# ─── Strip redundant .so from assets ─────────────────────────────────────────
add_custom_target(strip_asset_binaries
    COMMAND find "${ASSETS_DIR}" -name "*.so" -type f -delete
    COMMENT "Stripping redundant plugin .so from assets"
)
add_dependencies(strip_asset_binaries metadata_json)

# ─── Aggregate metadata target ───────────────────────────────────────────────
add_custom_target(metadata_done
    DEPENDS metadata_json modgui_resources strip_asset_binaries
    COMMENT "Metadata + modgui complete"
)
