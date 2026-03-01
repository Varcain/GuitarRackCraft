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
# cmake/modules/GeneratePluginMetadata.cmake — Refactored from generate_plugin_metadata.py
# =============================================================================

cmake_policy(SET CMP0007 NEW)

if(NOT PROJECT_ROOT OR NOT ASSETS_DIR)
    message(FATAL_ERROR "PROJECT_ROOT and ASSETS_DIR must be set")
endif()

set(THIRD_PARTY "${PROJECT_ROOT}/3rd_party")
set(GX_PLUGINS_DIR "${THIRD_PARTY}/GxPlugins.lv2.Android")
set(TRUNK_LV2_DIR "${THIRD_PARTY}/guitarix/trunk/src/LV2")
set(ASSETS_BASE_DIR "${ASSETS_DIR}/GxPlugins.lv2")

# We will collect everything in CMake variables first, then build JSON at the end.
set(ALL_DESCRIPTIONS "") # List of "name|desc"
set(ALL_THUMBNAILS "")   # List of "name|path"
set(ALL_AUTHORS "")     # List of "name|author"
set(ALL_CATEGORIES "")   # List of "name|cat"
set(AVAILABLE_PLUGINS "") # List of names

# --- Helper: Extract name and class from TTL ---
function(extract_from_ttl plugin_dir out_results)
    file(GLOB _ttls "${plugin_dir}/*.ttl")
    set(_found "")
    foreach(_ttl IN LISTS _ttls)
        get_filename_component(_ttl_name "${_ttl}" NAME)
        if(_ttl_name MATCHES "^(manifest|modgui|modguis)\\.ttl(\\.in)?$")
            continue()
        endif()
        
        file(READ "${_ttl}" _content)
        # Match doap:name "Name"
        string(REGEX MATCHALL "doap:name[ \t]+\"([^\"]+)\"" _names "${_content}")
        foreach(_name_match IN LISTS _names)
            string(REGEX REPLACE "doap:name[ \t]+\"([^\"]+)\"" "\\1" _name "${_name_match}")
            
            # Match lv2:Subclass (e.g. lv2:DistortionPlugin)
            string(REGEX MATCH "lv2:([A-Za-z0-9]+Plugin)" _class_match "${_content}")
            set(_class "Plugin")
            if(_class_match)
                string(REGEX REPLACE "lv2:([A-Za-z0-9]+Plugin)" "\\1" _class "${_class_match}")
            endif()
            
            list(APPEND _found "${_name}|${_class}")
        endforeach()
    endforeach()
    set(${out_results} "${_found}" PARENT_SCOPE)
endfunction()

# --- Helper: Parse README description ---
function(parse_readme readme_path out_name out_desc)
    file(READ "${readme_path}" _content)
    string(REPLACE "\r" "" _content "${_content}")
    string(REGEX REPLACE "\n+" "\n" _lines "${_content}")
    string(REPLACE "\n" ";" _lines "${_lines}")
    
    list(GET _lines 0 _header)
    if(_header MATCHES "^#[ \t]*([^ \t]+)\\.lv2")
        string(REGEX REPLACE "^#[ \t]*([^ \t]+)\\.lv2" "\\1" _name "${_header}")
        set(${out_name} "${_name}" PARENT_SCOPE)
        
        # Extract description
        set(_desc_lines "")
        list(REMOVE_AT _lines 0)
        foreach(_line IN LISTS _lines)
            string(STRIP "${_line}" _line)
            if(NOT _line OR _line MATCHES "^#" OR _line MATCHES "^!\\[")
                break()
            endif()
            list(APPEND _desc_lines "${_line}")
        endforeach()
        string(REPLACE ";" " " _desc "${_desc_lines}")
        set(${out_desc} "${_desc}" PARENT_SCOPE)
    else()
        set(${out_name} "" PARENT_SCOPE)
        set(${out_desc} "" PARENT_SCOPE)
    endif()
endfunction()

# --- Helper: Check for main binary in assets ---
function(has_binary_in_assets assets_plugin_dir out_has)
    file(GLOB _sos "${assets_plugin_dir}/*.so")
    set(_found FALSE)
    foreach(_so IN LISTS _sos)
        get_filename_component(_so_name "${_so}" NAME)
        if(NOT _so_name MATCHES "_ui\\.so$")
            set(_found TRUE)
            break()
        endif()
    endforeach()
    set(${out_has} ${_found} PARENT_SCOPE)
endfunction()

# --- Process GxPlugins ---
message(STATUS "Processing GxPlugins...")
file(GLOB _bundles LIST_DIRECTORIES true "${GX_PLUGINS_DIR}/*.lv2")
foreach(_bundle IN LISTS _bundles)
    if(NOT IS_DIRECTORY "${_bundle}")
        continue()
    endif()
    get_filename_component(_bundle_name "${_bundle}" NAME)
    string(REGEX REPLACE "\\.lv2$" "" _plugin_name_from_dir "${_bundle_name}")
    
    set(_plugin_name "${_plugin_name_from_dir}")
    set(_description "")
    
    if(EXISTS "${_bundle}/README.md")
        parse_readme("${_bundle}/README.md" _parsed_name _parsed_desc)
        if(_parsed_name)
            set(_plugin_name "${_parsed_name}")
            set(_description "${_parsed_desc}")
        endif()
    endif()
    
    # Extract category
    set(_category "Plugin")
    if(IS_DIRECTORY "${_bundle}/plugin")
        extract_from_ttl("${_bundle}/plugin" _ttl_results)
        if(_ttl_results)
            list(GET _ttl_results 0 _first)
            string(REGEX REPLACE "^.*\\|" "" _category "${_first}")
        endif()
    endif()
    
    list(APPEND ALL_DESCRIPTIONS "${_plugin_name}|${_description}")
    list(APPEND ALL_AUTHORS "${_plugin_name}|GxPlugins")
    list(APPEND ALL_CATEGORIES "${_plugin_name}|${_category}")
    
    # Thumbnail
    file(GLOB _pngs "${_bundle}/*.png")
    if(_pngs)
        list(GET _pngs 0 _png)
        get_filename_component(_png_name "${_png}" NAME)
        file(COPY "${_png}" DESTINATION "${ASSETS_BASE_DIR}/${_bundle_name}")
        list(APPEND ALL_THUMBNAILS "${_plugin_name}|GxPlugins.lv2/${_bundle_name}/${_png_name}")
    endif()
    
    # Binary check
    has_binary_in_assets("${ASSETS_BASE_DIR}/${_bundle_name}" _has_binary)
    if(_has_binary)
        list(APPEND AVAILABLE_PLUGINS "${_plugin_name}")
    endif()
endforeach()

# --- Process Trunk Plugins ---
if(IS_DIRECTORY "${TRUNK_LV2_DIR}")
    message(STATUS "Processing Trunk Plugins...")
    file(GLOB _bundles LIST_DIRECTORIES true "${TRUNK_LV2_DIR}/*.lv2")
    foreach(_bundle IN LISTS _bundles)
        if(NOT IS_DIRECTORY "${_bundle}")
            continue()
        endif()
        get_filename_component(_bundle_name "${_bundle}" NAME)
        
        extract_from_ttl("${_bundle}" _ttl_results)
        has_binary_in_assets("${ASSETS_BASE_DIR}/${_bundle_name}" _has_binary)
        
        # Check for thumbnail
        set(_thumb "")
        file(GLOB _modgui_pngs "${_bundle}/modgui/screenshot-*.png" "${_bundle}/modgui/thumbnail-*.png")
        if(_modgui_pngs)
            list(GET _modgui_pngs 0 _png)
            get_filename_component(_png_name "${_png}" NAME)
            file(COPY "${_png}" DESTINATION "${ASSETS_BASE_DIR}/${_bundle_name}")
            set(_thumb "GxPlugins.lv2/${_bundle_name}/${_png_name}")
        endif()
        
        if(NOT _ttl_results)
            string(REGEX REPLACE "\\.lv2$" "" _name "${_bundle_name}")
            set(_ttl_results "${_name}|Plugin")
        endif()
        
        foreach(_res IN LISTS _ttl_results)
            string(REGEX REPLACE "\\|.*$" "" _plugin_name "${_res}")
            string(REGEX REPLACE "^.*\\|" "" _category "${_res}")
            
            # Avoid duplicates if already added by GxPlugins
            set(_already_added FALSE)
            foreach(_existing IN LISTS ALL_AUTHORS)
                string(REGEX REPLACE "\\|.*$" "" _existing_name "${_existing}")
                if(_existing_name STREQUAL _plugin_name)
                    set(_already_added TRUE)
                    break()
                endif()
            endforeach()

            if(NOT _already_added)
                list(APPEND ALL_DESCRIPTIONS "${_plugin_name}|")
                list(APPEND ALL_AUTHORS "${_plugin_name}|Guitarix")
                list(APPEND ALL_CATEGORIES "${_plugin_name}|${_category}")
                if(_thumb)
                    list(APPEND ALL_THUMBNAILS "${_plugin_name}|${_thumb}")
                endif()
                if(_has_binary)
                    list(APPEND AVAILABLE_PLUGINS "${_plugin_name}")
                endif()
            endif()
        endforeach()
    endforeach()
endif()

# --- Helper: Scan External Assets ---
function(scan_external_assets dir_name author)
    set(_assets_dir "${ASSETS_DIR}/${dir_name}.lv2")
    if(NOT EXISTS "${_assets_dir}")
        message(STATUS "External assets dir NOT FOUND: ${_assets_dir}")
        return()
    endif()
    message(STATUS "Scanning external assets: ${dir_name} at ${_assets_dir}")
    extract_from_ttl("${_assets_dir}" _ttl_results)
    has_binary_in_assets("${_assets_dir}" _has_binary)
    
    if(NOT _has_binary)
        message(STATUS "  NO BINARY found in ${_assets_dir}")
    endif()

    # Check for thumbnail in modgui
    set(_thumb "")
    file(GLOB _modgui_pngs "${_assets_dir}/modgui/screenshot-*.png" "${_assets_dir}/modgui/thumbnail-*.png")
    if(_modgui_pngs)
        list(GET _modgui_pngs 0 _png)
        get_filename_component(_png_name "${_png}" NAME)
        set(_thumb "${dir_name}.lv2/${_png_name}")
        message(STATUS "  Found thumbnail: ${_thumb}")
    endif()

    foreach(_res IN LISTS _ttl_results)
        string(REGEX REPLACE "\\|.*$" "" _plugin_name "${_res}")
        string(REGEX REPLACE "^.*\\|" "" _category "${_res}")
        
        set(_already_added FALSE)
        foreach(_existing IN LISTS ALL_AUTHORS)
            string(REGEX REPLACE "\\|.*$" "" _existing_name "${_existing}")
            if(_existing_name STREQUAL _plugin_name)
                set(_already_added TRUE)
                break()
            endif()
        endforeach()

        if(NOT _already_added)
            message(STATUS "  Adding plugin: ${_plugin_name}")
            list(APPEND ALL_DESCRIPTIONS "${_plugin_name}|")
            list(APPEND ALL_AUTHORS "${_plugin_name}|${author}")
            list(APPEND ALL_CATEGORIES "${_plugin_name}|${_category}")
            if(_thumb)
                list(APPEND ALL_THUMBNAILS "${_plugin_name}|${_thumb}")
            endif()
            if(_has_binary)
                list(APPEND AVAILABLE_PLUGINS "${_plugin_name}")
            endif()
        endif()
    endforeach()
    set(ALL_DESCRIPTIONS "${ALL_DESCRIPTIONS}" PARENT_SCOPE)
    set(ALL_AUTHORS "${ALL_AUTHORS}" PARENT_SCOPE)
    set(ALL_CATEGORIES "${ALL_CATEGORIES}" PARENT_SCOPE)
    set(AVAILABLE_PLUGINS "${AVAILABLE_PLUGINS}" PARENT_SCOPE)
    set(ALL_THUMBNAILS "${ALL_THUMBNAILS}" PARENT_SCOPE)
endfunction()

scan_external_assets("neural_amp_modeler" "Neural Amp Modeler")
scan_external_assets("aidadsp" "Aida DSP")
scan_external_assets("AIDA-X" "Aida DSP")
scan_external_assets("ImpulseLoader" "brummer10")
scan_external_assets("XDarkTerror" "brummer10")
scan_external_assets("XTinyTerror" "brummer10")
scan_external_assets("CollisionDrive" "brummer10")
scan_external_assets("MetalTone" "brummer10")
scan_external_assets("GxCabSim" "brummer10")
scan_external_assets("PreAmps" "brummer10")
scan_external_assets("PowerAmps" "brummer10")
scan_external_assets("PreAmpImpulses" "brummer10")
scan_external_assets("PowerAmpImpulses" "brummer10")
scan_external_assets("FatFrog" "brummer10")
scan_external_assets("Neuralrack" "brummer10")
scan_external_assets("doubletracker" "Varcain")

# --- Helper: Populate a JSON object field from "key|value" list ---
function(json_set_map JSON_VAR FIELD)
    set(_json "${${JSON_VAR}}")
    set(_items ${ARGN})
    foreach(_item IN LISTS _items)
        string(FIND "${_item}" "|" _pos)
        string(SUBSTRING "${_item}" 0 ${_pos} _name)
        math(EXPR _val_start "${_pos} + 1")
        string(SUBSTRING "${_item}" ${_val_start} -1 _val)
        string(REPLACE "\"" "\\\"" _name_esc "${_name}")
        string(REPLACE "\"" "\\\"" _val_esc "${_val}")
        string(JSON _json SET "${_json}" "${FIELD}" "${_name_esc}" "\"${_val_esc}\"")
    endforeach()
    set(${JSON_VAR} "${_json}" PARENT_SCOPE)
endfunction()

# Build final JSON
set(JSON_DATA "{}")
string(JSON JSON_DATA SET "${JSON_DATA}" "descriptions" "{}")
string(JSON JSON_DATA SET "${JSON_DATA}" "thumbnails" "{}")
string(JSON JSON_DATA SET "${JSON_DATA}" "authors" "{}")
string(JSON JSON_DATA SET "${JSON_DATA}" "categories" "{}")
string(JSON JSON_DATA SET "${JSON_DATA}" "availablePlugins" "[]")

json_set_map(JSON_DATA "descriptions" "${ALL_DESCRIPTIONS}")
json_set_map(JSON_DATA "authors"      "${ALL_AUTHORS}")
json_set_map(JSON_DATA "categories"   "${ALL_CATEGORIES}")
json_set_map(JSON_DATA "thumbnails"   "${ALL_THUMBNAILS}")

set(_idx 0)
foreach(_item IN LISTS AVAILABLE_PLUGINS)
    string(REPLACE "\"" "\\\"" _item_esc "${_item}")
    string(JSON JSON_DATA SET "${JSON_DATA}" "availablePlugins" ${_idx} "\"${_item_esc}\"")
    math(EXPR _idx "${_idx} + 1")
endforeach()

# Write output
file(WRITE "${ASSETS_DIR}/../plugin_metadata.json" "${JSON_DATA}")
message(STATUS "Generated plugin_metadata.json at ${ASSETS_DIR}/../plugin_metadata.json")
