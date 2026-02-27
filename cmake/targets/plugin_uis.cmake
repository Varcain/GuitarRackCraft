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
# cmake/targets/plugin_uis.cmake — Build xputty + plugin UI .so files
#
# Builds:
#   1. libxcairo.a (xputty static widget library with PNG resources)
#   2. Guitarix trunk plugin UI .so files
#   3. GxPlugins.lv2.Android UI .so files
#
# Replaces: scripts/build_x11_uis.sh (~335 lines)
# =============================================================================

set(_xputty_src "${THIRD_PARTY}/guitarix/trunk/src/LV2/xputty")
set(_gx_plugins_src "${THIRD_PARTY}/GxPlugins.lv2.Android")
set(_trunk_lv2 "${THIRD_PARTY}/guitarix/trunk/src/LV2")
set(_ui_build "${PROJECT_ROOT}/build/x11_ui")
set(_ui_output_dir "${CMAKE_BINARY_DIR}/plugin_uis")
set(_lv2_headers "${APP_ROOT}/cpp/libs/lv2/include")

# ─── 1. Build xputty (libxcairo.a) ───────────────────────────────────────────

# Collect source files (skip mswin)
file(GLOB _xputty_cpp "${_xputty_src}/*.cpp")
file(GLOB _xputty_widgets "${_xputty_src}/widgets/*.cpp")
file(GLOB _xputty_dialogs "${_xputty_src}/dialogs/*.cpp")
file(GLOB _xputty_xdgmime "${_xputty_src}/xdgmime/*.c")

set(_xputty_sources ${_xputty_cpp} ${_xputty_widgets} ${_xputty_dialogs} ${_xputty_xdgmime})
# Remove mswin files
list(FILTER _xputty_sources EXCLUDE REGEX "mswin")

# PNG resource embedding via ld -r -b binary
file(GLOB _xputty_pngs "${_xputty_src}/resources/*.png")
set(_xputty_res_objs "")
foreach(_png IN LISTS _xputty_pngs)
    get_filename_component(_png_name "${_png}" NAME_WE)
    get_filename_component(_png_file "${_png}" NAME)
    get_filename_component(_png_dir "${_png}" DIRECTORY)
    set(_res_obj "${_ui_build}/xputty/res_${_png_name}.o")

    add_custom_command(
        OUTPUT "${_res_obj}"
        COMMAND ${NDK_LD} -r -b binary -m aarch64linux "${_png_file}" -o "${_res_obj}"
        WORKING_DIRECTORY "${_png_dir}"
        DEPENDS "${_png}"
        COMMENT "Embedding ${_png_file}"
    )
    list(APPEND _xputty_res_objs "${_res_obj}")
endforeach()

add_library(xputty_lib STATIC ${_xputty_sources} ${_xputty_res_objs})
target_include_directories(xputty_lib PRIVATE
    "${_xputty_src}/header"
    "${_xputty_src}/header/widgets"
    "${_xputty_src}/header/dialogs"
    "${_xputty_src}/resources"
    "${_xputty_src}/xdgmime"
    "${X11_SYSROOT}/include"
    "${X11_SYSROOT}/include/cairo"
)
target_compile_options(xputty_lib PRIVATE -fPIC -DANDROID -O2)
set_target_properties(xputty_lib PROPERTIES OUTPUT_NAME "xcairo")
add_dependencies(xputty_lib x11_sysroot)

# ─── 2. Build guitarix trunk plugin UIs ──────────────────────────────────────

set(_ui_cxxflags -fPIC -DANDROID -O2 -std=c++17 -fvisibility=hidden -DNO_XSHM)
set(_ui_ldflags -shared -Wl,-z,noexecstack -Wl,-z,relro,-z,now)
set(_ui_includes
    "${LV2_COMPAT_DIR}"
    "${_xputty_src}/header"
    "${_xputty_src}/header/widgets"
    "${_xputty_src}/header/dialogs"
    "${_xputty_src}/resources"
    "${_xputty_src}/lv2_plugin"
    "${X11_SYSROOT}/include"
    "${X11_SYSROOT}/include/cairo"
    "${_lv2_headers}/lv2"
    "${_lv2_headers}"
)

set(_trunk_ui_targets "")

file(GLOB _trunk_plugin_dirs "${_trunk_lv2}/gx*.lv2" "${_trunk_lv2}/Gx*.lv2")
foreach(_pdir IN LISTS _trunk_plugin_dirs)
    if(NOT IS_DIRECTORY "${_pdir}")
        continue()
    endif()
    get_filename_component(_pname "${_pdir}" NAME)

    # Find *_ui.cpp
    file(GLOB _ui_cpps "${_pdir}/*_ui.cpp")
    if(NOT _ui_cpps)
        continue()
    endif()
    list(GET _ui_cpps 0 _ui_cpp)
    get_filename_component(_ui_base "${_ui_cpp}" NAME_WE)
    set(_so_name "${_ui_base}.so")

    # Find matching asset directory
    set(_asset_dir "")
    file(GLOB _asset_dirs "${ASSETS_DIR}/GxPlugins.lv2/*.lv2")
    foreach(_ad IN LISTS _asset_dirs)
        # Check if TTL references this .so
        file(GLOB _ad_ttls "${_ad}/*.ttl")
        foreach(_ttl IN LISTS _ad_ttls)
            file(STRINGS "${_ttl}" _binary_refs REGEX "${_so_name}")
            if(_binary_refs)
                set(_asset_dir "${_ad}")
                break()
            endif()
        endforeach()
        if(_asset_dir)
            break()
        endif()
        # Fallback: case-insensitive name match
        get_filename_component(_ad_base "${_ad}" NAME_WE)
        string(TOLOWER "${_ad_base}" _ad_lower)
        string(REPLACE "_" "" _ad_lower "${_ad_lower}")
        string(REGEX REPLACE "_ui$" "" _so_prefix "${_ui_base}")
        string(TOLOWER "${_so_prefix}" _so_lower)
        string(REPLACE "_" "" _so_lower "${_so_lower}")
        if(_ad_lower STREQUAL _so_lower)
            set(_asset_dir "${_ad}")
            break()
        endif()
    endforeach()

    if(NOT _asset_dir)
        continue()
    endif()

    set(_target "trunk_ui_${_ui_base}")

    add_library(${_target} SHARED "${_ui_cpp}")
    target_include_directories(${_target} PRIVATE ${_ui_includes} "${_pdir}")
    target_compile_options(${_target} PRIVATE ${_ui_cxxflags})
    target_link_options(${_target} PRIVATE ${_ui_ldflags})
    target_link_libraries(${_target} PRIVATE
        -Wl,--whole-archive xputty_lib -Wl,--no-whole-archive
        "${X11_SYSROOT}/lib/libcairo.a"
        "${X11_SYSROOT}/lib/libpixman-1.a"
        "${X11_SYSROOT}/lib/libpng.a"
        -L"${X11_SYSROOT}/lib"
        X11 xcb Xau
        xshm_stub
        m z log dl
    )
    # Build into a dir that mirrors the assets structure
    file(RELATIVE_PATH _rel_asset_path "${ASSETS_DIR}" "${_asset_dir}")
    set_target_properties(${_target} PROPERTIES
        OUTPUT_NAME "${_ui_base}"
        SUFFIX ".so"
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${_ui_output_dir}/${_rel_asset_path}"
    )
    add_dependencies(${_target} xputty_lib x11_sysroot lv2_libs)

    list(APPEND _trunk_ui_targets "${_target}")
endforeach()

# ─── 3. Build GxPlugins.lv2.Android X11 UIs ──────────────────────────────────

set(_gx_ui_targets "")

if(IS_DIRECTORY "${_gx_plugins_src}")
    file(GLOB _gx_bundles_ui "${_gx_plugins_src}/*.lv2")
    foreach(_bundle IN LISTS _gx_bundles_ui)
        if(NOT IS_DIRECTORY "${_bundle}")
            continue()
        endif()
        get_filename_component(_bundle_name "${_bundle}" NAME)

        # Find gui/*x11ui.c
        file(GLOB _ui_cs "${_bundle}/gui/*x11ui.c")
        if(NOT _ui_cs)
            continue()
        endif()
        list(GET _ui_cs 0 _ui_c)

        # Get .so name from TTL
        set(_so_name "")
        file(GLOB _plugin_ttls "${_bundle}/plugin/*.ttl")
        foreach(_ttl IN LISTS _plugin_ttls)
            file(STRINGS "${_ttl}" _binary_lines REGEX "guiext:binary")
            foreach(_line IN LISTS _binary_lines)
                string(REGEX MATCH "<([^>]+)>" _match "${_line}")
                if(CMAKE_MATCH_1)
                    set(_so_name "${CMAKE_MATCH_1}")
                    break()
                endif()
            endforeach()
            if(_so_name)
                break()
            endif()
        endforeach()

        if(NOT _so_name)
            continue()
        endif()

        # Find matching asset dir
        set(_asset_dir "${ASSETS_DIR}/GxPlugins.lv2/${_bundle_name}")
        if(NOT IS_DIRECTORY "${_asset_dir}")
            continue()
        endif()

        # PNG resource embedding
        file(GLOB _gui_pngs "${_bundle}/gui/*.png")
        set(_gx_res_objs "")
        foreach(_png IN LISTS _gui_pngs)
            get_filename_component(_png_name "${_png}" NAME_WE)
            get_filename_component(_png_file "${_png}" NAME)
            get_filename_component(_png_dir "${_png}" DIRECTORY)
            set(_res_obj "${_ui_build}/gx_uis/${_bundle_name}/res_${_png_name}.o")

            file(MAKE_DIRECTORY "${_ui_build}/gx_uis/${_bundle_name}")
            add_custom_command(
                OUTPUT "${_res_obj}"
                COMMAND ${NDK_LD} -r -b binary -m aarch64linux "${_png_file}" -o "${_res_obj}"
                WORKING_DIRECTORY "${_png_dir}"
                DEPENDS "${_png}"
                COMMENT "Embedding ${_bundle_name}/${_png_file}"
            )
            list(APPEND _gx_res_objs "${_res_obj}")
        endforeach()

        get_filename_component(_so_base "${_so_name}" NAME_WE)
        set(_target "gx_ui_${_so_base}")

        add_library(${_target} SHARED "${_ui_c}" ${_gx_res_objs})
        target_include_directories(${_target} PRIVATE
            "${LV2_COMPAT_DIR}"
            "${X11_SYSROOT}/include"
            "${X11_SYSROOT}/include/cairo"
            "${_lv2_headers}"
            "${_bundle}" "${_bundle}/dsp" "${_bundle}/plugin" "${_bundle}/gui"
        )
        target_compile_options(${_target} PRIVATE
            -fPIC -DANDROID -O2 -Wno-unused-parameter -DNO_XSHM)
        target_link_options(${_target} PRIVATE ${_ui_ldflags})
        target_link_libraries(${_target} PRIVATE
            "${X11_SYSROOT}/lib/libcairo.a"
            "${X11_SYSROOT}/lib/libpixman-1.a"
            "${X11_SYSROOT}/lib/libpng.a"
            -L"${X11_SYSROOT}/lib"
            X11 xcb Xau
            xshm_stub
            m z log dl
        )
        # Build into a dir that mirrors the assets structure
        file(RELATIVE_PATH _rel_asset_path "${ASSETS_DIR}" "${_asset_dir}")
        set_target_properties(${_target} PROPERTIES
            OUTPUT_NAME "${_so_base}"
            SUFFIX ".so"
            PREFIX ""
            LIBRARY_OUTPUT_DIRECTORY "${_ui_output_dir}/${_rel_asset_path}"
        )
        add_dependencies(${_target} x11_sysroot lv2_libs)

        list(APPEND _gx_ui_targets "${_target}")
    endforeach()
endif()

lv2_sync_to_jnilibs(plugin_uis_jnisync "${_ui_output_dir}" "${_trunk_ui_targets};${_gx_ui_targets}"
    INCLUDE_PATTERN "_ui\\\\.so$"
)

add_custom_target(plugin_uis_done
    DEPENDS plugin_uis_jnisync
    COMMENT "All plugin UIs built and synced to jniLibs"
)
