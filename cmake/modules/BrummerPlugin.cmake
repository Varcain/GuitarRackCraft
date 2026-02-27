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
# cmake/modules/BrummerPlugin.cmake — Build Brummer10's LV2 plugins (libxputty based)
# =============================================================================

# Define the common flags once
set(BRUMMER_C_FLAGS
    -fPIC -DANDROID -O2 -std=c17
    -DNO_XSHM -DUSE_LD=1 -fvisibility=hidden
    -Wno-unused-parameter -Wno-unused-result -D_FORTIFY_SOURCE=2
)

set(BRUMMER_XPUTTY_FLAGS
    -fPIC -DANDROID -O2 -Wno-unused-parameter -Wno-format-security -DNO_XSHM -DUSE_LD=1
)

# ─── Setup XPutty with PNG resources ─────────────────────────────────────────
function(brummer_setup_xputty TARGET_NAME XPUTTY_SRC BUILD_DIR PNG_DIR)
    set(_xputty_build "${BUILD_DIR}/xputty")
    set(_xputty_res "${XPUTTY_SRC}/resources")
    file(MAKE_DIRECTORY "${_xputty_build}")
    file(WRITE "${_xputty_build}/config.h" "#ifndef USE_LD\n#define USE_LD 1\n#endif\n")

    # Copy PNGs to xputty/resources
    if(PNG_DIR)
        file(GLOB _pngs "${PNG_DIR}/*.png")
        foreach(_png IN LISTS _pngs)
            get_filename_component(_png_name "${_png}" NAME)
            configure_file("${_png}" "${_xputty_res}/${_png_name}" COPYONLY)
        endforeach()
    endif()

    # Generate xresources.h
    file(GLOB _all_res_pngs "${_xputty_res}/*.png")
    set(_xres_content "")
    foreach(_png IN LISTS _all_res_pngs)
        get_filename_component(_png_name_we "${_png}" NAME_WE)
        string(APPEND _xres_content "EXTLD(${_png_name_we}_png)\n")
    endforeach()
    file(WRITE "${_xputty_res}/xresources.h" "${_xres_content}")

    # PNG resource objects
    set(_res_objs "")
    foreach(_png IN LISTS _all_res_pngs)
        get_filename_component(_png_name_we "${_png}" NAME_WE)
        get_filename_component(_png_file "${_png}" NAME)
        set(_obj "${_xputty_build}/res_${_png_name_we}.o")
        add_custom_command(
            OUTPUT "${_obj}"
            COMMAND ${NDK_LD} -r -b binary -m aarch64linux "${_png_file}" -o "${_obj}"
            WORKING_DIRECTORY "${_xputty_res}"
            DEPENDS "${_png}"
            COMMENT "Embedding resources for ${TARGET_NAME}"
        )
        list(APPEND _res_objs "${_obj}")
    endforeach()

    # Collect xputty sources
    file(GLOB _xp_srcs "${XPUTTY_SRC}/*.c")
    file(GLOB _xp_widgets "${XPUTTY_SRC}/widgets/*.c")
    file(GLOB _xp_dialogs "${XPUTTY_SRC}/dialogs/*.c")
    file(GLOB _xp_xdg "${XPUTTY_SRC}/xdgmime/*.c")
    set(_xp_all ${_xp_srcs} ${_xp_widgets} ${_xp_dialogs} ${_xp_xdg})
    list(FILTER _xp_all EXCLUDE REGEX "mswin")

    add_library(${TARGET_NAME} STATIC ${_xp_all} ${_res_objs})
    target_include_directories(${TARGET_NAME} PRIVATE
        "${_xputty_build}"
        "${XPUTTY_SRC}/header"
        "${XPUTTY_SRC}/header/widgets"
        "${XPUTTY_SRC}/header/dialogs"
        "${_xputty_res}"
        "${XPUTTY_SRC}/xdgmime"
        "${X11_SYSROOT}/include"
        "${X11_SYSROOT}/include/cairo"
    )
    target_compile_options(${TARGET_NAME} PRIVATE ${BRUMMER_XPUTTY_FLAGS})
    add_dependencies(${TARGET_NAME} x11_sysroot)
endfunction()

# ─── Add UI Target ───────────────────────────────────────────────────────────
function(brummer_add_ui_target TARGET_NAME OUTPUT_NAME SOURCE BUILD_DIR XPUTTY_TARGET LV2_COMPAT_DIR)
    cmake_parse_arguments(ARG "" "" "INCLUDES;DEFINITIONS;DEPENDS" ${ARGN})

    add_library(${TARGET_NAME} SHARED "${SOURCE}")
    
    target_include_directories(${TARGET_NAME} PRIVATE
        "${BUILD_DIR}/xputty"
        "${LV2_COMPAT_DIR}"
        "${LV2_INCLUDE}"
        "${ARG_INCLUDES}"
        "${X11_SYSROOT}/include"
        "${X11_SYSROOT}/include/cairo"
    )
    target_compile_options(${TARGET_NAME} PRIVATE ${BRUMMER_C_FLAGS} ${ARG_DEFINITIONS})
    target_link_options(${TARGET_NAME} PRIVATE
        -shared -Wl,-z,noexecstack -Wl,-z,relro,-z,now
        -Wl,--gc-sections -Wl,--exclude-libs,ALL)
    
    target_link_libraries(${TARGET_NAME} PRIVATE
        -Wl,--whole-archive ${XPUTTY_TARGET} -Wl,--no-whole-archive
        "${X11_SYSROOT}/lib/libcairo.a"
        "${X11_SYSROOT}/lib/libpixman-1.a"
        "${X11_SYSROOT}/lib/libpng.a"
        -L"${X11_SYSROOT}/lib"
        X11 xcb Xau
        xshm_stub
        m z log dl
    )
    
    set_target_properties(${TARGET_NAME} PROPERTIES
        OUTPUT_NAME "${OUTPUT_NAME}"
        SUFFIX ".so"
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${BUILD_DIR}"
    )
    add_dependencies(${TARGET_NAME} ${XPUTTY_TARGET} x11_sysroot lv2_libs ${ARG_DEPENDS})
endfunction()

# ─── Combined Macro for simple plugins ───────────────────────────────────────
function(brummer_add_plugin)
    set(options)
    set(oneValueArgs NAME OUTPUT_NAME SOURCE_DIR DSP_SOURCE UI_SOURCE ASSETS_DIR PNG_DIR)
    set(multiValueArgs DSP_INCLUDES UI_INCLUDES UI_DEFINITIONS)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_OUTPUT_NAME)
        set(ARG_OUTPUT_NAME "${ARG_NAME}")
    endif()

    set(_build_dir "${PROJECT_ROOT}/build/${ARG_NAME}")
    set(_xputty_src "${ARG_SOURCE_DIR}/libxputty/xputty")
    set(_lv2_compat "${_build_dir}/lv2_compat")

    file(MAKE_DIRECTORY "${_build_dir}")
    file(MAKE_DIRECTORY "${ARG_ASSETS_DIR}")
    generate_lv2_compat_headers("${_lv2_compat}")

    # --- DSP ---
    set(_dsp_target "${ARG_NAME}_dsp")
    add_library(${_dsp_target} SHARED "${ARG_DSP_SOURCE}")
    lv2_set_dsp_properties(${_dsp_target} "${ARG_OUTPUT_NAME}" "${_build_dir}")
    target_include_directories(${_dsp_target} PRIVATE
        "${_lv2_compat}"
        "${LV2_INCLUDE}"
        "${ARG_DSP_INCLUDES}"
    )
    add_dependencies(${_dsp_target} lv2_libs)
    lv2_strip_and_save_debug(${_dsp_target} "${_build_dir}/${ARG_OUTPUT_NAME}.so")

    # --- XPutty ---
    set(_xputty_target "xputty_${ARG_NAME}")
    brummer_setup_xputty(${_xputty_target} "${_xputty_src}" "${_build_dir}" "${ARG_PNG_DIR}")

    # --- UI ---
    set(_ui_target "${ARG_NAME}_ui")
    brummer_add_ui_target(${_ui_target} "${ARG_OUTPUT_NAME}_ui" "${ARG_UI_SOURCE}" "${_build_dir}" "${_xputty_target}" "${_lv2_compat}"
        INCLUDES
            "${_xputty_src}/header"
            "${_xputty_src}/header/widgets"
            "${_xputty_src}/header/dialogs"
            "${_xputty_src}/resources"
            "${_xputty_src}/lv2_plugin"
            "${_xputty_src}/xdgmime"
            "${ARG_UI_INCLUDES}"
        DEFINITIONS ${ARG_UI_DEFINITIONS}
    )

    # --- Sync (stamp-based) ---
    lv2_sync_dsp_ui(
        NAME ${ARG_NAME}
        OUTPUT_NAME ${ARG_OUTPUT_NAME}
        BUILD_DIR "${_build_dir}"
        ASSETS_DIR "${ARG_ASSETS_DIR}"
        DSP_TARGET ${_dsp_target}
        UI_TARGET ${_ui_target}
    )
endfunction()
