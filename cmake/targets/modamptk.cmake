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
# cmake/targets/modamptk.cmake — Build ModularAmpToolKit LV2 plugins
# =============================================================================

set(_matk_src      "${THIRD_PARTY}/ModularAmpToolKit.lv2")
set(_matk_build    "${PROJECT_ROOT}/build/modamptk")
set(_matk_xputty   "${_matk_src}/libxputty/xputty")
set(_matk_lv2_compat "${_matk_build}/lv2_compat")

file(MAKE_DIRECTORY "${_matk_build}")
generate_lv2_compat_headers("${_matk_lv2_compat}")

# ─── Shared: xputty static lib ───────────────────────────────────────────────
set(_matk_xputty_target "xputty_matk")
brummer_setup_xputty(${_matk_xputty_target} "${_matk_xputty}" "${_matk_build}" "${_matk_src}/resources")

# ─── Plugin 1: PreAmps ───────────────────────────────────────────────────────
set(_pa_dir    "${_matk_src}/PreAmps")
set(_pa_build  "${_matk_build}/preamps")
set(_pa_assets "${ASSETS_DIR}/PreAmps.lv2")

file(MAKE_DIRECTORY "${_pa_assets}")
configure_file("${_pa_dir}/manifest.ttl" "${_pa_assets}/manifest.ttl" COPYONLY)
configure_file("${_pa_dir}/PreAmps.ttl"  "${_pa_assets}/PreAmps.ttl" COPYONLY)

add_library(preamps_dsp SHARED "${_pa_dir}/PreAmps.cpp")
target_include_directories(preamps_dsp PRIVATE "${_matk_lv2_compat}" "${LV2_INCLUDE}" "${_pa_dir}" "${_pa_dir}/DSP")
lv2_set_dsp_properties(preamps_dsp "PreAmps" "${_pa_build}")
lv2_strip_and_save_debug(preamps_dsp "${_pa_build}/PreAmps.so")

brummer_add_ui_target(preamps_ui "PreAmps_ui" "${_pa_dir}/PreAmps.c" "${_pa_build}" "${_matk_xputty_target}" "${_matk_lv2_compat}"
    INCLUDES "${_pa_dir}" "${_matk_xputty}/header" "${_matk_xputty}/header/widgets" "${_matk_xputty}/header/dialogs" "${_matk_xputty}/resources" "${_matk_xputty}/lv2_plugin" "${_matk_xputty}/xdgmime"
)
lv2_sync_dsp_ui(
    NAME preamps
    OUTPUT_NAME PreAmps
    BUILD_DIR "${_pa_build}"
    ASSETS_DIR "${_pa_assets}"
    DSP_TARGET preamps_dsp
    UI_TARGET preamps_ui
)

# ─── Plugin 2: PowerAmps ──────────────────────────────────────────────────────
set(_pwa_dir    "${_matk_src}/PowerAmps")
set(_pwa_build  "${_matk_build}/poweramps")
set(_pwa_assets "${ASSETS_DIR}/PowerAmps.lv2")

file(MAKE_DIRECTORY "${_pwa_assets}")
configure_file("${_pwa_dir}/manifest.ttl" "${_pwa_assets}/manifest.ttl" COPYONLY)
configure_file("${_pwa_dir}/poweramps.ttl" "${_pwa_assets}/poweramps.ttl" COPYONLY)

add_library(poweramps_dsp SHARED "${_pwa_dir}/poweramps.cpp")
target_include_directories(poweramps_dsp PRIVATE "${_matk_lv2_compat}" "${LV2_INCLUDE}" "${_pwa_dir}" "${_pwa_dir}/dsp")
lv2_set_dsp_properties(poweramps_dsp "poweramps" "${_pwa_build}")
lv2_strip_and_save_debug(poweramps_dsp "${_pwa_build}/poweramps.so")

brummer_add_ui_target(poweramps_ui "poweramps_ui" "${_pwa_dir}/poweramps.c" "${_pwa_build}" "${_matk_xputty_target}" "${_matk_lv2_compat}"
    INCLUDES "${_pwa_dir}" "${_matk_xputty}/header" "${_matk_xputty}/header/widgets" "${_matk_xputty}/header/dialogs" "${_matk_xputty}/resources" "${_matk_xputty}/lv2_plugin" "${_matk_xputty}/xdgmime"
)
lv2_sync_dsp_ui(
    NAME poweramps
    OUTPUT_NAME poweramps
    BUILD_DIR "${_pwa_build}"
    ASSETS_DIR "${_pwa_assets}"
    DSP_TARGET poweramps_dsp
    UI_TARGET poweramps_ui
)

# ─── Plugin 3: PreAmpImpulses ────────────────────────────────────────────────
set(_pai_dir    "${_matk_src}/PreAmpImpulses")
set(_pai_build  "${_matk_build}/preampimpulses")
set(_pai_assets "${ASSETS_DIR}/PreAmpImpulses.lv2")

file(MAKE_DIRECTORY "${_pai_assets}")
configure_file("${_pai_dir}/manifest.ttl" "${_pai_assets}/manifest.ttl" COPYONLY)
configure_file("${_pai_dir}/PreAmpImpulses.ttl" "${_pai_assets}/PreAmpImpulses.ttl" COPYONLY)

add_library(preampimpulses_dsp SHARED "${_pai_dir}/PreAmpImpulses.cpp")
target_include_directories(preampimpulses_dsp PRIVATE "${_matk_lv2_compat}" "${LV2_INCLUDE}" "${_pai_dir}" "${_pai_dir}/DSP" "${_pai_dir}/DSP/zita-resampler-1.1.0" "${_pai_dir}/DSP/zita-convolver" "${FFTW3_PREFIX}/include")
lv2_set_dsp_properties(preampimpulses_dsp "PreAmpImpulses" "${_pai_build}")
target_link_libraries(preampimpulses_dsp PRIVATE "${FFTW3_PREFIX}/lib/libfftw3f.a")
add_dependencies(preampimpulses_dsp fftw3)
lv2_strip_and_save_debug(preampimpulses_dsp "${_pai_build}/PreAmpImpulses.so")

brummer_add_ui_target(preampimpulses_ui "PreAmpImpulses_ui" "${_pai_dir}/PreAmpImpulses.c" "${_pai_build}" "${_matk_xputty_target}" "${_matk_lv2_compat}"
    INCLUDES "${_pai_dir}" "${_matk_xputty}/header" "${_matk_xputty}/header/widgets" "${_matk_xputty}/header/dialogs" "${_matk_xputty}/resources" "${_matk_xputty}/lv2_plugin" "${_matk_xputty}/xdgmime"
)
lv2_sync_dsp_ui(
    NAME preampimpulses
    OUTPUT_NAME PreAmpImpulses
    BUILD_DIR "${_pai_build}"
    ASSETS_DIR "${_pai_assets}"
    DSP_TARGET preampimpulses_dsp
    UI_TARGET preampimpulses_ui
)

# ─── Plugin 4: PowerAmpImpulses ──────────────────────────────────────────────
set(_pwai_dir    "${_matk_src}/PowerAmpImpulses")
set(_pwai_build  "${_matk_build}/powerampimpulses")
set(_pwai_assets "${ASSETS_DIR}/PowerAmpImpulses.lv2")

file(MAKE_DIRECTORY "${_pwai_assets}")
configure_file("${_pwai_dir}/manifest.ttl" "${_pwai_assets}/manifest.ttl" COPYONLY)
configure_file("${_pwai_dir}/PowerAmpImpulses.ttl" "${_pwai_assets}/PowerAmpImpulses.ttl" COPYONLY)

add_library(powerampimpulses_dsp SHARED "${_pwai_dir}/PowerAmpImpulses.cpp")
target_include_directories(powerampimpulses_dsp PRIVATE "${_matk_lv2_compat}" "${LV2_INCLUDE}" "${_pwai_dir}" "${_pwai_dir}/DSP" "${_pwai_dir}/DSP/zita-resampler-1.1.0" "${_pwai_dir}/DSP/zita-convolver" "${_pwai_dir}/DSP/amp_ir" "${FFTW3_PREFIX}/include")
lv2_set_dsp_properties(powerampimpulses_dsp "PowerAmpImpulses" "${_pwai_build}")
target_link_libraries(powerampimpulses_dsp PRIVATE "${FFTW3_PREFIX}/lib/libfftw3f.a")
add_dependencies(powerampimpulses_dsp fftw3)
lv2_strip_and_save_debug(powerampimpulses_dsp "${_pwai_build}/PowerAmpImpulses.so")

brummer_add_ui_target(powerampimpulses_ui "PowerAmpImpulses_ui" "${_pwai_dir}/PowerAmpImpulses.c" "${_pwai_build}" "${_matk_xputty_target}" "${_matk_lv2_compat}"
    INCLUDES "${_pwai_dir}" "${_matk_xputty}/header" "${_matk_xputty}/header/widgets" "${_matk_xputty}/header/dialogs" "${_matk_xputty}/resources" "${_matk_xputty}/lv2_plugin" "${_matk_xputty}/xdgmime"
)
lv2_sync_dsp_ui(
    NAME powerampimpulses
    OUTPUT_NAME PowerAmpImpulses
    BUILD_DIR "${_pwai_build}"
    ASSETS_DIR "${_pwai_assets}"
    DSP_TARGET powerampimpulses_dsp
    UI_TARGET powerampimpulses_ui
)

# ─── Aggregate target ──────────────────────────────────────────────────
add_custom_target(modamptk_done
    DEPENDS preamps_done poweramps_done preampimpulses_done powerampimpulses_done)
