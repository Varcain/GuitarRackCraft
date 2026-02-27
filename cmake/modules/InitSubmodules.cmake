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
# cmake/modules/InitSubmodules.cmake — Helper to init submodules and apply patches
# =============================================================================

function(setup_submodules)
    set(_patch_script "${CMAKE_BINARY_DIR}/scripts/ApplyPatches.cmake")
    file(WRITE "${_patch_script}"
    [=[
        # Apply patches from 3rd_party/patches/<submodule>/
        set(PATCHES_DIR "${PROJECT_ROOT}/3rd_party/patches")
        if(NOT EXISTS "${PATCHES_DIR}")
            return()
        endif()

        file(GLOB_RECURSE _patches "${PATCHES_DIR}/*.patch")
        list(SORT _patches)
        foreach(_patch IN LISTS _patches)
            file(RELATIVE_PATH _rel "${PATCHES_DIR}" "${_patch}")
            get_filename_component(_submod_rel "${_rel}" DIRECTORY)
            set(_submod_dir "${PROJECT_ROOT}/3rd_party/${_submod_rel}")
            if(IS_DIRECTORY "${_submod_dir}")
                get_filename_component(_patch_name "${_patch}" NAME)
                message(STATUS "  Applying ${_patch_name} to 3rd_party/${_submod_rel}")
                execute_process(
                    COMMAND patch -p1 --forward --no-backup-if-mismatch
                    INPUT_FILE "${_patch}"
                    WORKING_DIRECTORY "${_submod_dir}"
                    RESULT_VARIABLE _result
                    OUTPUT_QUIET ERROR_QUIET)
            endif()
        endforeach()
    ]=])

    add_custom_target(init_submodules
        COMMAND git -C "${PROJECT_ROOT}" submodule update --init --recursive
        COMMAND ${CMAKE_COMMAND}
            -DPROJECT_ROOT=${PROJECT_ROOT}
            -P "${_patch_script}"
        COMMENT "Initializing submodules and applying patches"
    )
endfunction()
