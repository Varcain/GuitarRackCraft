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
# cmake/modules/LV2PluginUtils.cmake — Common utilities for LV2 plugin builds
# =============================================================================

# ─── Sync .so files to jniLibs (stamp-based) ────────────────────────────────
# Usage: lv2_sync_to_jnilibs(TARGET_NAME SOURCE_DIR DEPENDS_LIST
#            [EXCLUDE_PATTERN pattern] [INCLUDE_PATTERN pattern])
#
# SOURCE_DIR: directory to scan for .so files (build dir)
# .so files go ONLY to jniLibs (not duplicated into assets)
function(lv2_sync_to_jnilibs TARGET_NAME SOURCE_DIR DEPENDS_LIST)
    cmake_parse_arguments(ARG "" "EXCLUDE_PATTERN;INCLUDE_PATTERN;ASSETS_COPY_DIR" "" ${ARGN})

    set(_sync_script "${CMAKE_BINARY_DIR}/scripts/Sync_${TARGET_NAME}.cmake")
    set(_stamp "${CMAKE_BINARY_DIR}/stamps/${TARGET_NAME}.stamp")

    set(_logic "")
    if(ARG_INCLUDE_PATTERN)
        set(_logic "${_logic}
            if(NOT \"\${_name}\" MATCHES \"${ARG_INCLUDE_PATTERN}\")
                continue()
            endif()
        ")
    else()
        # Default excludes if no explicit include pattern
        set(_logic "${_logic}
            if(\"\${_name}\" MATCHES \"_ui\\\\.so$\" OR \"\${_name}\" MATCHES \"_debug\\\\.so$\")
                continue()
            endif()
        ")
    endif()

    if(ARG_EXCLUDE_PATTERN)
        set(_logic "${_logic}
            if(\"\${_name}\" MATCHES \"${ARG_EXCLUDE_PATTERN}\")
                continue()
            endif()
        ")
    endif()

    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/scripts" "${CMAKE_BINARY_DIR}/stamps")

    file(WRITE "${_sync_script}"
    "
        file(MAKE_DIRECTORY \"\${JNILIBS_DIR}\")
        file(GLOB_RECURSE _sos \"\${SOURCE_DIR}/*.so\")
        foreach(_so IN LISTS _sos)
            get_filename_component(_name \"\${_so}\" NAME)
            ${_logic}
            execute_process(COMMAND \${CMAKE_COMMAND} -E copy_if_different \"\${_so}\" \"\${JNILIBS_DIR}/lib\${_name}\")
            execute_process(COMMAND \${NDK_STRIP} --strip-unneeded \"\${JNILIBS_DIR}/lib\${_name}\")
        endforeach()
        file(WRITE \"\${STAMP}\" \"\")
    ")

    set(_cmd_args
        -DSOURCE_DIR=${SOURCE_DIR}
        -DJNILIBS_DIR=${JNILIBS_DIR}
        -DNDK_STRIP=${NDK_STRIP}
        -DSTAMP=${_stamp}
    )

    add_custom_command(
        OUTPUT "${_stamp}"
        COMMAND ${CMAKE_COMMAND} ${_cmd_args} -P "${_sync_script}"
        DEPENDS ${DEPENDS_LIST}
        COMMENT "Syncing ${TARGET_NAME} DSP .so to jniLibs"
    )
    add_custom_target(${TARGET_NAME} DEPENDS "${_stamp}")
endfunction()

# ─── Sync DSP + UI .so to jniLibs (stamp-based) ─────────────────────────────
# .so files go ONLY to jniLibs (not duplicated into assets/lv2).
#
# Usage: lv2_sync_dsp_ui(
#     NAME <target_prefix>         # e.g. "neuralrack"
#     OUTPUT_NAME <so_base>        # e.g. "Neuralrack" → Neuralrack.so, Neuralrack_ui.so
#     BUILD_DIR <build_dir>        # directory containing built .so files
#     ASSETS_DIR <assets_dir>      # destination assets/lv2/Plugin.lv2 directory (TTL only)
#     DSP_TARGET <dsp_target>      # CMake target for the DSP .so
#     UI_TARGET <ui_target>        # CMake target for the UI .so
# )
function(lv2_sync_dsp_ui)
    cmake_parse_arguments(ARG "" "NAME;OUTPUT_NAME;BUILD_DIR;ASSETS_DIR;DSP_TARGET;UI_TARGET" "" ${ARGN})
    set(_stamp "${CMAKE_BINARY_DIR}/stamps/${ARG_NAME}_sync.stamp")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/stamps")

    add_custom_command(
        OUTPUT "${_stamp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${JNILIBS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ARG_BUILD_DIR}/${ARG_OUTPUT_NAME}.so" "${JNILIBS_DIR}/lib${ARG_OUTPUT_NAME}.so"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ARG_BUILD_DIR}/${ARG_OUTPUT_NAME}_ui.so" "${JNILIBS_DIR}/lib${ARG_OUTPUT_NAME}_ui.so"
        COMMAND ${NDK_STRIP} --strip-unneeded "${JNILIBS_DIR}/lib${ARG_OUTPUT_NAME}.so" || true
        COMMAND ${NDK_STRIP} --strip-unneeded "${JNILIBS_DIR}/lib${ARG_OUTPUT_NAME}_ui.so" || true
        COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
        DEPENDS ${ARG_DSP_TARGET} ${ARG_UI_TARGET}
        COMMENT "Syncing ${ARG_OUTPUT_NAME} to jniLibs"
    )
    add_custom_target(${ARG_NAME}_sync DEPENDS "${_stamp}")
    add_custom_target(${ARG_NAME}_done DEPENDS ${ARG_NAME}_sync)
endfunction()

# ─── Standard DSP compilation options ─────────────────────────────────────────
macro(lv2_set_dsp_properties TARGET_NAME OUTPUT_NAME OUTPUT_DIR)
    set_target_properties(${TARGET_NAME} PROPERTIES
        OUTPUT_NAME "${OUTPUT_NAME}"
        SUFFIX ".so"
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_DIR}"
    )
    target_compile_options(${TARGET_NAME} PRIVATE
        -fPIC -DANDROID -O2 -std=c++17 -ffast-math -funroll-loops
        -DNDEBUG -fvisibility=hidden
    )
    target_link_options(${TARGET_NAME} PRIVATE
        -shared -Wl,--exclude-libs,ALL -Wl,--gc-sections -Wl,-z,noexecstack
        -Wl,--no-undefined
    )
    target_link_libraries(${TARGET_NAME} PRIVATE m log)
endmacro()

# ─── Strip and Save Debug Copy ────────────────────────────────────────────────
function(lv2_strip_and_save_debug TARGET_NAME BINARY_PATH)
    get_filename_component(_dir "${BINARY_PATH}" DIRECTORY)
    get_filename_component(_name_we "${BINARY_PATH}" NAME_WE)
    get_filename_component(_ext "${BINARY_PATH}" EXT)
    set(_debug_path "${_dir}/${_name_we}_debug${_ext}")

    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${BINARY_PATH}" "${_debug_path}"
        COMMAND ${NDK_STRIP} --strip-unneeded "${BINARY_PATH}"
        COMMENT "Stripping ${TARGET_NAME} (debug copy saved to ${_debug_path})"
    )
endfunction()
