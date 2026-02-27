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
# cmake/targets/impulseloader.cmake — Build ImpulseLoader LV2 plugin
# =============================================================================

set(_il_src       "${THIRD_PARTY}/ImpulseLoader")
set(_il_plugin    "${_il_src}/ImpulseLoader")
set(_il_build     "${PROJECT_ROOT}/build/impulseloader")
set(_il_assets    "${ASSETS_DIR}/ImpulseLoader.lv2")
set(_il_xputty    "${_il_src}/libxputty/xputty")

# ─── Phase 1: Copy TTL ──────────────────────────────────────────────────────
file(MAKE_DIRECTORY "${_il_assets}")
if(EXISTS "${_il_plugin}/lv2/manifest.ttl")
    configure_file("${_il_plugin}/lv2/manifest.ttl" "${_il_assets}/manifest.ttl" COPYONLY)
endif()
if(EXISTS "${_il_plugin}/lv2/ImpulseLoader.ttl")
    configure_file("${_il_plugin}/lv2/ImpulseLoader.ttl" "${_il_assets}/ImpulseLoader.ttl" COPYONLY)
endif()

# ─── Phase 2a: libfftconvolver.a (shared function) ──────────────────────────
add_fftconvolver_library(il_fftconvolver "${_il_src}" "${_il_plugin}" "${SNDFILE_PREFIX}/include" shared_libsndfile)

# ─── Phase 2a: libzita-resampler.a (shared function) ────────────────────────
add_zita_resampler_library(il_zita_resampler "${_il_plugin}")

# ─── Phase 2b: DSP plugin (ImpulseLoader.so) ────────────────────────────────
set(_il_lv2_compat "${_il_build}/lv2_compat")
generate_lv2_compat_headers("${_il_lv2_compat}")

add_library(impulseloader_dsp SHARED
    "${_il_plugin}/lv2/ImpulseLoader.cpp"
)
target_include_directories(impulseloader_dsp PRIVATE
    "${_il_lv2_compat}" "${LV2_INCLUDE}" "${_il_plugin}" "${_il_plugin}/engine" "${_il_plugin}/lv2"
    "${_il_plugin}/zita-resampler-1.1.0" "${_il_src}/FFTConvolver"
    "${SNDFILE_PREFIX}/include"
)
target_compile_options(impulseloader_dsp PRIVATE
    -fPIC -DANDROID -O3 -std=c++17 -funroll-loops -DNDEBUG -DUSE_ATOM -fvisibility=hidden
    -Wno-sign-compare -Wno-reorder -Wno-infinite-recursion -fdata-sections)
target_link_options(impulseloader_dsp PRIVATE
    -shared -Wl,--exclude-libs,ALL -Wl,--gc-sections -Wl,-z,noexecstack -Wl,--no-undefined)
target_link_libraries(impulseloader_dsp PRIVATE
    il_fftconvolver il_zita_resampler
    "${SNDFILE_PREFIX}/lib/libsndfile.a"
    m log)
set_target_properties(impulseloader_dsp PROPERTIES
    OUTPUT_NAME "ImpulseLoader" SUFFIX ".so" PREFIX "" LIBRARY_OUTPUT_DIRECTORY "${_il_build}")
add_dependencies(impulseloader_dsp shared_libsndfile lv2_libs)
lv2_strip_and_save_debug(impulseloader_dsp "${_il_build}/ImpulseLoader.so")

# ─── Phase 2c: UI plugin (ImpulseLoader_ui.so) ──────────────────────────────
set(_xputty_target "xputty_il")
brummer_setup_xputty(${_xputty_target} "${_il_xputty}" "${_il_build}" "${_il_plugin}/resources")

brummer_add_ui_target(impulseloader_ui "ImpulseLoader_ui" "${_il_plugin}/gui/ImpulseLoader.c" "${_il_build}" "${_xputty_target}" "${_il_lv2_compat}"
    INCLUDES
        "${_il_plugin}" "${_il_plugin}/gui" "${_il_plugin}/lv2"
        "${_il_xputty}/header" "${_il_xputty}/header/widgets" "${_il_xputty}/header/dialogs"
        "${_il_xputty}/resources" "${_il_xputty}/lv2_plugin" "${_il_xputty}/xdgmime"
    DEFINITIONS -DUSE_ATOM
)

# ─── Phase 3: Sync (stamp-based) ────────────────────────────────────────────
lv2_sync_dsp_ui(
    NAME impulseloader
    OUTPUT_NAME ImpulseLoader
    BUILD_DIR "${_il_build}"
    ASSETS_DIR "${_il_assets}"
    DSP_TARGET impulseloader_dsp
    UI_TARGET impulseloader_ui
)
