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
# cmake/targets/trunk_plugins.cmake — Build guitarix trunk LV2 plugins
#
# Parses wscript files at configure time to determine sources/deps,
# then creates add_library(SHARED) targets for each plugin.
#
# Replaces: scripts/build_gx_trunk_plugins.sh (~416 lines)
# =============================================================================

set(_trunk_src   "${THIRD_PARTY}/guitarix/trunk/src/LV2")
set(_trunk_dest  "${ASSETS_DIR}/GxPlugins.lv2")
set(_trunk_build_dir "${CMAKE_BINARY_DIR}/trunk_plugins")
set(_faust_gen   "${_trunk_src}/faust-generated")
set(_zita_res    "${THIRD_PARTY}/guitarix/trunk/src/zita-resampler-1.1.0")
set(_zita_conv   "${THIRD_PARTY}/guitarix/trunk/src/zita-convolver")
set(_gx_dsp      "${_trunk_src}/DSP")
set(_eigen_dir   "${THIRD_PARTY}/eigen")
set(_guitarix_root "${THIRD_PARTY}/guitarix/trunk")

# Plugins to skip
set(_skip_plugins "gx_livelooper")

# Create config.h for convolver
file(WRITE "${_guitarix_root}/config.h"
    "/* Minimal config.h for Android builds */\n"
    "#define ZITA_CONVOLVER_VERSION 3\n")

# Download Eigen3 if missing
if(NOT EXISTS "${_eigen_dir}/Eigen")
    message(STATUS "Downloading Eigen3 (header-only)...")
    execute_process(
        COMMAND git clone --depth 1 --branch 3.4.0
            https://gitlab.com/libeigen/eigen.git "${_eigen_dir}"
        RESULT_VARIABLE _eigen_result)
    if(NOT _eigen_result EQUAL 0)
        message(WARNING "Failed to download Eigen3 — some plugins may fail")
    endif()
endif()

# Write the wscript parser script
set(_wscript_parser "${CMAKE_BINARY_DIR}/scripts/parse_wscript.py")
file(WRITE "${_wscript_parser}"
[=[
"""Parse a guitarix wscript to extract lv2_base, source, use, includes."""
import re, sys, json
wscript = sys.argv[1]
text = open(wscript).read()
m = re.search(r'bld\.lv2\(\s*(.*?)\)', text, re.DOTALL)
if not m:
    print('{}')
    sys.exit(0)
block = m.group(1)
# lv2_base
bm = re.search(r"lv2_base\s*=\s*'([^']+)'", text)
if not bm:
    bm = re.search(r"lv2_base\s*=\s*\"([^\"]+)\"", text)
base = bm.group(1) if bm else ''
# source
sm = re.search(r"source\s*=\s*\[([^\]]+)\]", block)
sources = re.findall(r"'([^']+)'", sm.group(1)) if sm else []
# use
um = re.search(r"use\s*=\s*\[([^\]]+)\]", block)
uses = re.findall(r"'([^']+)'", um.group(1)) if um else []
# includes
im = re.search(r"includes\s*=\s*\[([^\]]+)\]", block)
includes = re.findall(r"'([^']+)'", im.group(1)) if im else []
print(json.dumps({'base': base, 'sources': sources, 'uses': uses, 'includes': includes}))
]=])

# ─── Phase 1: Copy TTL + modgui ──────────────────────────────────────────────
file(GLOB _trunk_bundles "${_trunk_src}/*.lv2")
foreach(_bundle IN LISTS _trunk_bundles)
    if(NOT IS_DIRECTORY "${_bundle}")
        continue()
    endif()
    get_filename_component(_dir_name "${_bundle}" NAME)
    get_filename_component(_base_name "${_bundle}" NAME_WE)

    # Check skip list
    list(FIND _skip_plugins "${_base_name}" _skip_idx)
    if(NOT _skip_idx EQUAL -1)
        continue()
    endif()

    set(_target_dir "${_trunk_dest}/${_dir_name}")
    file(MAKE_DIRECTORY "${_target_dir}")

    # Process manifest.ttl.in → manifest.ttl
    if(EXISTS "${_bundle}/manifest.ttl.in")
        file(READ "${_bundle}/manifest.ttl.in" _manifest_content)
        string(REPLACE "@LIB_EXT@" ".so" _manifest_content "${_manifest_content}")
        file(WRITE "${_target_dir}/manifest.ttl" "${_manifest_content}")
    elseif(EXISTS "${_bundle}/manifest.ttl")
        configure_file("${_bundle}/manifest.ttl" "${_target_dir}/manifest.ttl" COPYONLY)
    endif()

    # Find UI cpp name for TTL rewriting
    file(GLOB _ui_cpps "${_bundle}/*_ui.cpp")
    set(_ui_so_name "")
    if(_ui_cpps)
        list(GET _ui_cpps 0 _ui_cpp)
        get_filename_component(_ui_base "${_ui_cpp}" NAME_WE)
        set(_ui_so_name "${_ui_base}.so")
    endif()

    # Copy other TTL files, rewriting guiext:binary if needed
    file(GLOB _ttls "${_bundle}/*.ttl")
    foreach(_ttl IN LISTS _ttls)
        get_filename_component(_ttl_name "${_ttl}" NAME)
        if(_ttl_name STREQUAL "manifest.ttl")
            continue()  # Already handled above
        endif()
        if(_ui_so_name)
            file(READ "${_ttl}" _ttl_content)
            string(REGEX REPLACE
                "guiext:binary <[^>]*_gui\\.so>"
                "guiext:binary <${_ui_so_name}>"
                _ttl_content "${_ttl_content}")
            file(WRITE "${_target_dir}/${_ttl_name}" "${_ttl_content}")
        else()
            configure_file("${_ttl}" "${_target_dir}/${_ttl_name}" COPYONLY)
        endif()
    endforeach()

    # Copy modgui
    if(IS_DIRECTORY "${_bundle}/modgui")
        file(COPY "${_bundle}/modgui/" DESTINATION "${_target_dir}/modgui/")
    endif()
endforeach()

# ─── Phase 2: Build DSP .so per plugin ───────────────────────────────────────

set(_trunk_cxxflags
    -DANDROID -fPIC -O2 -std=c++17
    -fomit-frame-pointer -ffunction-sections -fdata-sections
    -fno-rtti -fno-exceptions -fvisibility=hidden
)

set(_trunk_built_sos "")

foreach(_bundle IN LISTS _trunk_bundles)
    if(NOT IS_DIRECTORY "${_bundle}")
        continue()
    endif()
    get_filename_component(_dir_name "${_bundle}" NAME)
    get_filename_component(_base_name "${_bundle}" NAME_WE)

    list(FIND _skip_plugins "${_base_name}" _skip_idx)
    if(NOT _skip_idx EQUAL -1)
        continue()
    endif()

    # Parse wscript
    if(NOT EXISTS "${_bundle}/wscript")
        continue()
    endif()

    execute_process(
        COMMAND python3 "${_wscript_parser}" "${_bundle}/wscript"
        OUTPUT_VARIABLE _wscript_json
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _parse_result)

    if(NOT _parse_result EQUAL 0 OR NOT _wscript_json)
        continue()
    endif()

    # Parse JSON output
    string(JSON _lv2_base GET "${_wscript_json}" "base")
    if(NOT _lv2_base)
        continue()
    endif()

    # Get sources array
    string(JSON _src_count LENGTH "${_wscript_json}" "sources")
    set(_sources "")
    if(_src_count GREATER 0)
        math(EXPR _src_last "${_src_count} - 1")
        foreach(_i RANGE 0 ${_src_last})
            string(JSON _src GET "${_wscript_json}" "sources" ${_i})
            list(APPEND _sources "${_bundle}/${_src}")
        endforeach()
    endif()

    if(NOT _sources)
        continue()
    endif()

    # Get uses array
    string(JSON _use_count LENGTH "${_wscript_json}" "uses")
    set(_uses "")
    if(_use_count GREATER 0)
        math(EXPR _use_last "${_use_count} - 1")
        foreach(_i RANGE 0 ${_use_last})
            string(JSON _use GET "${_wscript_json}" "uses" ${_i})
            list(APPEND _uses "${_use}")
        endforeach()
    endif()

    # Get includes array
    string(JSON _inc_count LENGTH "${_wscript_json}" "includes")
    set(_extra_includes "")
    if(_inc_count GREATER 0)
        math(EXPR _inc_last "${_inc_count} - 1")
        foreach(_i RANGE 0 ${_inc_last})
            string(JSON _inc GET "${_wscript_json}" "includes" ${_i})
            list(APPEND _extra_includes "${_bundle}/${_inc}")
        endforeach()
    endif()

    # Determine dependencies from uses
    set(_needs_resampler FALSE)
    set(_needs_fftw3 FALSE)
    set(_needs_convolver FALSE)
    set(_needs_exceptions FALSE)

    foreach(_use IN LISTS _uses)
        if(_use MATCHES "ZITA_RESAMPLER|GX_RESAMPLER|GX_CONVOLVER|ZITA_CONVOLVER")
            set(_needs_resampler TRUE)
        endif()
        if(_use MATCHES "FFTW3|CONVOLVER")
            set(_needs_fftw3 TRUE)
        endif()
        if(_use MATCHES "GX_CONVOLVER|ZITA_CONVOLVER")
            set(_needs_convolver TRUE)
        endif()
        if(_use MATCHES "CONVOLVER|SIGC")
            set(_needs_exceptions TRUE)
        endif()
    endforeach()

    # Check for try/catch in source and all included .cc files (e.g. gx_detune)
    file(GLOB _all_bundle_srcs "${_bundle}/*.cc" "${_bundle}/*.cpp")
    foreach(_src IN LISTS _all_bundle_srcs)
        if(EXISTS "${_src}")
            file(STRINGS "${_src}" _try_lines REGEX "try \\{")
            if(_try_lines)
                set(_needs_exceptions TRUE)
            endif()
        endif()
    endforeach()

    # Add resampler sources if needed
    if(_needs_resampler)
        # Check if resampler.cc is #included inline
        set(_inline_resampler FALSE)
        foreach(_src IN LISTS _sources)
            if(EXISTS "${_src}")
                file(STRINGS "${_src}" _resamp_inc REGEX "#include.*resampler\\.cc")
                if(_resamp_inc)
                    set(_inline_resampler TRUE)
                    break()
                endif()
            endif()
        endforeach()
        if(NOT _inline_resampler)
            list(APPEND _sources
                "${_zita_res}/resampler.cc"
                "${_zita_res}/resampler-table.cc"
                "${_gx_dsp}/gx_resampler.cc")
        endif()
    endif()

    # Add convolver sources if needed
    if(_needs_convolver)
        # Check if gx_convolver.cc is #included inline
        set(_inline_conv FALSE)
        file(GLOB _bundle_ccs "${_bundle}/*.cc" "${_bundle}/*.cpp")
        foreach(_cc IN LISTS _bundle_ccs)
            file(STRINGS "${_cc}" _conv_inc REGEX "#include.*gx_convolver\\.cc")
            if(_conv_inc)
                set(_inline_conv TRUE)
                break()
            endif()
        endforeach()
        if(NOT _inline_conv)
            list(APPEND _sources "${_gx_dsp}/gx_convolver.cc")
        endif()
        list(APPEND _sources "${_zita_conv}/zita-convolver.cc")
    endif()

    # Create target
    set(_target_name "trunk_dsp_${_lv2_base}")
    set(_dest_dir "${_trunk_dest}/${_dir_name}")

    add_library(${_target_name} SHARED ${_sources})
    target_include_directories(${_target_name} PRIVATE
        "${LV2_COMPAT_DIR}"
        "${LV2_INCLUDE}"
        "${_faust_gen}"
        "${FFTW3_PREFIX}/include"
        "${_zita_conv}"
        "${_gx_dsp}"
        "${_eigen_dir}"
        "${_bundle}"
        ${_extra_includes}
    )
    if(_needs_resampler)
        target_include_directories(${_target_name} PRIVATE "${_zita_res}")
    endif()

    target_compile_options(${_target_name} PRIVATE ${_trunk_cxxflags})
    # Force-include the compat header for min/max
    target_compile_options(${_target_name} PRIVATE
        -include "${LV2_COMPAT_DIR}/gx_android_compat.h")

    if(_needs_exceptions)
        target_compile_options(${_target_name} PRIVATE -fexceptions)
    endif()

    target_link_options(${_target_name} PRIVATE
        -Wl,--gc-sections -Wl,-z,noexecstack)
    target_link_libraries(${_target_name} PRIVATE m log)

    if(_needs_fftw3)
        target_link_directories(${_target_name} PRIVATE "${FFTW3_PREFIX}/lib")
        target_link_libraries(${_target_name} PRIVATE fftw3f)
        add_dependencies(${_target_name} fftw3)
    endif()

    set_target_properties(${_target_name} PROPERTIES
        OUTPUT_NAME "${_lv2_base}"
        SUFFIX ".so"
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${_trunk_build_dir}/${_dir_name}"
    )
    add_dependencies(${_target_name} lv2_libs)

    list(APPEND _trunk_built_sos "${_target_name}")
endforeach()

# ─── Phase 3: Sync to assets + jniLibs ───────────────────────────────────────
lv2_sync_to_jnilibs(trunk_plugins_sync "${_trunk_build_dir}" "${_trunk_built_sos}"
    ASSETS_COPY_DIR "${_trunk_dest}")

add_custom_target(trunk_plugins_done DEPENDS trunk_plugins_sync)
