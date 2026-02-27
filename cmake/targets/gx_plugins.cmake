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
# cmake/targets/gx_plugins.cmake — Build GxPlugins.lv2.Android DSP .so files
#
# Three phases:
#   1. Copy TTL + modgui to assets (configure time)
#   2. Build DSP .so via add_library(SHARED) per plugin
#   3. Sync .so to jniLibs with lib prefix
#
# Replaces: scripts/build_gxplugins.sh (~178 lines)
# =============================================================================

set(_gx_src  "${THIRD_PARTY}/GxPlugins.lv2.Android")
set(_gx_dest "${ASSETS_DIR}/GxPlugins.lv2")
set(_gx_build_dir "${CMAKE_BINARY_DIR}/gx_plugins")

# ─── Phase 1: Copy TTL + modgui at configure time ────────────────────────────
file(GLOB _gx_bundles "${_gx_src}/*.lv2")
foreach(_bundle IN LISTS _gx_bundles)
    if(NOT IS_DIRECTORY "${_bundle}")
        continue()
    endif()
    get_filename_component(_name "${_bundle}" NAME)
    set(_target_dir "${_gx_dest}/${_name}")
    file(MAKE_DIRECTORY "${_target_dir}")

    # GxPlugins.lv2.Android keeps plugin TTLs in plugin/ and modgui TTLs in MOD/.
    # Stage them at bundle root to match LV2 bundle layout expected by lilv.
    set(_manifest_src "")

    # Prefer MOD/manifest.ttl because it includes modgui.ttl in rdfs:seeAlso.
    if(EXISTS "${_bundle}/MOD/manifest.ttl")
        set(_manifest_src "${_bundle}/MOD/manifest.ttl")
    elseif(EXISTS "${_bundle}/plugin/manifest.ttl")
        set(_manifest_src "${_bundle}/plugin/manifest.ttl")
    elseif(EXISTS "${_bundle}/manifest.ttl")
        set(_manifest_src "${_bundle}/manifest.ttl")
    endif()
    if(_manifest_src)
        configure_file("${_manifest_src}" "${_target_dir}/manifest.ttl" COPYONLY)
    endif()

    # Copy plugin TTLs (contains doap:name + guiext:X11UI).
    file(GLOB _plugin_ttls "${_bundle}/plugin/*.ttl")
    foreach(_ttl IN LISTS _plugin_ttls)
        get_filename_component(_ttl_name "${_ttl}" NAME)
        if(_ttl_name STREQUAL "manifest.ttl")
            continue()
        endif()
        configure_file("${_ttl}" "${_target_dir}/${_ttl_name}" COPYONLY)
    endforeach()

    # Copy modgui TTLs (modgui.ttl / modguis.ttl) if present.
    foreach(_mod_ttl_name IN ITEMS modgui.ttl modguis.ttl)
        if(EXISTS "${_bundle}/MOD/${_mod_ttl_name}")
            configure_file("${_bundle}/MOD/${_mod_ttl_name}" "${_target_dir}/${_mod_ttl_name}" COPYONLY)
        endif()
    endforeach()

    # Copy bundle-root TTLs (fallback for atypical bundles).
    file(GLOB _root_ttls "${_bundle}/*.ttl")
    foreach(_ttl IN LISTS _root_ttls)
        get_filename_component(_ttl_name "${_ttl}" NAME)
        if(_ttl_name STREQUAL "manifest.ttl")
            continue()
        endif()
        configure_file("${_ttl}" "${_target_dir}/${_ttl_name}" COPYONLY)
    endforeach()

    # Copy modgui resources.
    if(IS_DIRECTORY "${_bundle}/MOD/modgui")
        file(COPY "${_bundle}/MOD/modgui/" DESTINATION "${_target_dir}/modgui/")
    endif()
endforeach()

# ─── Phase 2: Build DSP .so per plugin ───────────────────────────────────────

# Common compile flags
set(_gx_cxxflags
    -DANDROID -fPIC -O2 -Wall -std=c++17
    -fomit-frame-pointer -ffunction-sections -fdata-sections
    -fno-rtti -fno-exceptions
)
set(_gx_includes
    "${LV2_COMPAT_DIR}"
    "${LV2_INCLUDE}"
)
set(_gx_ldflags
    -shared -lm -llog
    -Wl,--gc-sections -Wl,-z,noexecstack
)

# Track all built plugin .so files for the sync step
set(_gx_built_sos "")

foreach(_bundle IN LISTS _gx_bundles)
    if(NOT IS_DIRECTORY "${_bundle}")
        continue()
    endif()
    get_filename_component(_bundle_name "${_bundle}" NAME)
    get_filename_component(_plugin_name "${_bundle}" NAME_WE)  # without .lv2

    # Find main source file
    set(_source "")
    string(TOLOWER "${_plugin_name}" _plugin_lower)
    if(EXISTS "${_bundle}/plugin/gx_${_plugin_lower}.cpp")
        set(_source "${_bundle}/plugin/gx_${_plugin_lower}.cpp")
    elseif(EXISTS "${_bundle}/plugin/gx_${_plugin_name}.cpp")
        set(_source "${_bundle}/plugin/gx_${_plugin_name}.cpp")
    else()
        file(GLOB _cpps "${_bundle}/plugin/*.cpp")
        if(_cpps)
            list(GET _cpps 0 _source)
        endif()
    endif()

    if(NOT _source)
        continue()
    endif()

    # Determine output name from Makefile or source basename
    set(_output_name "")
    if(EXISTS "${_bundle}/Makefile")
        file(STRINGS "${_bundle}/Makefile" _makefile_lines REGEX "^NAME = ")
        if(_makefile_lines)
            list(GET _makefile_lines 0 _name_line)
            string(REGEX REPLACE "^NAME = " "" _output_name "${_name_line}")
            string(STRIP "${_output_name}" _output_name)
        endif()
    endif()
    if(NOT _output_name)
        get_filename_component(_output_name "${_source}" NAME_WE)
    endif()

    set(_target_name "gx_dsp_${_output_name}")
    set(_so_path "${_gx_dest}/${_bundle_name}/${_output_name}.so")

    # Collect sources
    set(_sources "${_source}")

    # Check for resampler
    file(STRINGS "${_source}" _resampler_include REGEX "#include.*resampler\\.cc")
    if(NOT _resampler_include)
        # Check for separate resampler source
        if(EXISTS "${_bundle}/dsp/zita-resampler-1.1.0/resampler.cc")
            list(APPEND _sources "${_bundle}/dsp/zita-resampler-1.1.0/resampler.cc")
        elseif(EXISTS "${_bundle}/dsp/resampler.cc")
            list(APPEND _sources "${_bundle}/dsp/resampler.cc")
        endif()
    endif()

    # Build includes
    set(_plugin_includes ${_gx_includes}
        "${_bundle}" "${_bundle}/dsp" "${_bundle}/plugin")
    if(IS_DIRECTORY "${_bundle}/dsp/zita-resampler-1.1.0")
        list(APPEND _plugin_includes "${_bundle}/dsp/zita-resampler-1.1.0")
    endif()

    # Create shared library target
    add_library(${_target_name} SHARED ${_sources})
    target_include_directories(${_target_name} PRIVATE ${_plugin_includes})
    target_compile_options(${_target_name} PRIVATE ${_gx_cxxflags})
    target_link_options(${_target_name} PRIVATE
        -Wl,--gc-sections -Wl,-z,noexecstack)
    target_link_libraries(${_target_name} PRIVATE m log)
    set_target_properties(${_target_name} PROPERTIES
        OUTPUT_NAME "${_output_name}"
        SUFFIX ".so"
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${_gx_build_dir}/${_bundle_name}"
    )
    add_dependencies(${_target_name} lv2_libs)

    list(APPEND _gx_built_sos "${_target_name}")
endforeach()

# ─── Phase 3: Sync to assets + jniLibs ───────────────────────────────────────
# Copy DSP .so from build dir to assets (for metadata) and jniLibs (for APK)
lv2_sync_to_jnilibs(gx_plugins_sync "${_gx_build_dir}" "${_gx_built_sos}")

add_custom_target(gx_plugins_done DEPENDS gx_plugins_sync)
