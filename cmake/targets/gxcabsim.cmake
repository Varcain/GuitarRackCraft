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
# cmake/targets/gxcabsim.cmake — Build GxCabSim LV2 plugin
# =============================================================================

set(_gcs_src       "${THIRD_PARTY}/GxCabSim.lv2")
set(_gcs_build     "${PROJECT_ROOT}/build/gxcabsim")
set(_gcs_assets    "${ASSETS_DIR}/GxCabSim.lv2")

# ─── Phase 1: Copy TTL ──────────────────────────────────────────────────
file(MAKE_DIRECTORY "${_gcs_assets}")
if(EXISTS "${_gcs_src}/plugin/manifest.ttl")
    configure_file("${_gcs_src}/plugin/manifest.ttl" "${_gcs_assets}/manifest.ttl" COPYONLY)
endif()
if(EXISTS "${_gcs_src}/plugin/gx_cabsim.ttl")
    configure_file("${_gcs_src}/plugin/gx_cabsim.ttl" "${_gcs_assets}/gx_cabsim.ttl" COPYONLY)
endif()

# ─── Phase 2a: DSP plugin (gx_cabsim.so) ────────────────────────────────
set(_gcs_lv2_compat "${_gcs_build}/lv2_compat")
generate_lv2_compat_headers("${_gcs_lv2_compat}")

add_library(gxcabsim_dsp SHARED
    "${_gcs_src}/plugin/gx_cabsim.cpp"
)
target_include_directories(gxcabsim_dsp PRIVATE
    "${_gcs_lv2_compat}"
    "${LV2_INCLUDE}"
    "${_gcs_src}/plugin"
    "${_gcs_src}/dsp"
    "${_gcs_src}/dsp/zita-resampler-1.1.0"
)
lv2_set_dsp_properties(gxcabsim_dsp "gx_cabsim" "${_gcs_build}")
add_dependencies(gxcabsim_dsp lv2_libs)
lv2_strip_and_save_debug(gxcabsim_dsp "${_gcs_build}/gx_cabsim.so")

# ─── Phase 2b: UI plugin (gx_cabsim_ui.so) ──────────────────────────────
set(_gcs_ui_build "${_gcs_build}/ui")
file(MAKE_DIRECTORY "${_gcs_ui_build}")

# Embed PNG resources
set(_gcs_pngs "${_gcs_src}/gui/pedal.png" "${_gcs_src}/gui/pswitch.png")
set(_gcs_res_objs "")
foreach(_png IN LISTS _gcs_pngs)
    get_filename_component(_name "${_png}" NAME_WE)
    get_filename_component(_file "${_png}" NAME)
    set(_obj "${_gcs_ui_build}/res_${_name}.o")
    add_custom_command(
        OUTPUT "${_obj}"
        COMMAND ${NDK_LD} -r -b binary -m aarch64linux "${_file}" -o "${_obj}"
        WORKING_DIRECTORY "${_gcs_src}/gui"
        DEPENDS "${_png}"
        COMMENT "Embedding GCS ${_file}"
    )
    list(APPEND _gcs_res_objs "${_obj}")
endforeach()

add_library(gxcabsim_ui SHARED
    "${_gcs_src}/gui/gx_cabsim_x11ui.c"
    ${_gcs_res_objs}
)
target_include_directories(gxcabsim_ui PRIVATE
    "${_gcs_lv2_compat}"
    "${LV2_INCLUDE}"
    "${_gcs_src}/plugin"
    "${X11_SYSROOT}/include"
    "${X11_SYSROOT}/include/cairo"
)
target_compile_options(gxcabsim_ui PRIVATE
    -fPIC -DANDROID -O2 -std=c17 -fvisibility=hidden
    -Wno-unused-parameter -Wno-unused-result
    -Wno-incompatible-function-pointer-types)
target_link_options(gxcabsim_ui PRIVATE
    -shared -Wl,-z,noexecstack -Wl,-z,relro,-z,now
    -Wl,--gc-sections -Wl,--exclude-libs,ALL)
target_link_libraries(gxcabsim_ui PRIVATE
    "${X11_SYSROOT}/lib/libcairo.a"
    "${X11_SYSROOT}/lib/libpixman-1.a"
    "${X11_SYSROOT}/lib/libpng.a"
    -L"${X11_SYSROOT}/lib"
    X11 xcb Xau
    xshm_stub
    m z log dl
)
set_target_properties(gxcabsim_ui PROPERTIES
    OUTPUT_NAME "gx_cabsim_ui"
    SUFFIX ".so"
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${_gcs_build}"
)
add_dependencies(gxcabsim_ui x11_sysroot lv2_libs)

# ─── Phase 3: Sync (stamp-based) ─────────────────────────────────────────────
lv2_sync_dsp_ui(
    NAME gxcabsim
    OUTPUT_NAME gx_cabsim
    BUILD_DIR "${_gcs_build}"
    ASSETS_DIR "${_gcs_assets}"
    DSP_TARGET gxcabsim_dsp
    UI_TARGET gxcabsim_ui
)
