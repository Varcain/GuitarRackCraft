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
# cmake/targets/neuralrack.cmake — Build NeuralRack LV2 plugin
# =============================================================================

set(_nr_src       "${THIRD_PARTY}/NeuralRack")
set(_nr_plugin    "${_nr_src}/NeuralRack")
set(_nr_build     "${PROJECT_ROOT}/build/neuralrack")
set(_nr_assets    "${ASSETS_DIR}/Neuralrack.lv2")
set(_nr_xputty    "${_nr_src}/libxputty/xputty")
set(_nr_na_dir    "${_nr_src}/NeuralAudio/NeuralAudio")
set(_nr_dep_dir   "${_nr_src}/NeuralAudio/deps")

# ─── Phase 1: Copy TTL ──────────────────────────────────────────────────────
file(MAKE_DIRECTORY "${_nr_assets}")
if(EXISTS "${_nr_plugin}/lv2/manifest.ttl")
    configure_file("${_nr_plugin}/lv2/manifest.ttl" "${_nr_assets}/manifest.ttl" COPYONLY)
endif()
if(EXISTS "${_nr_plugin}/lv2/NeuralRack.ttl")
    configure_file("${_nr_plugin}/lv2/NeuralRack.ttl" "${_nr_assets}/NeuralRack.ttl" COPYONLY)
endif()

# ─── Phase 2a: libneuralaudio.a ──────────────────────────────────────────────
add_library(neuralaudio STATIC
    "${_nr_na_dir}/NeuralModel.cpp"
    "${_nr_na_dir}/RTNeuralLoader.cpp"
    "${_nr_dep_dir}/RTNeural/RTNeural/RTNeural.cpp"
)
target_include_directories(neuralaudio PRIVATE
    "${_nr_na_dir}" "${_nr_dep_dir}/RTNeural/" "${_nr_plugin}"
    "${_nr_dep_dir}/math_approx/include/" "${_nr_dep_dir}"
    "${_nr_dep_dir}/NeuralAmpModelerCore/Dependencies/nlohmann/"
    "${_nr_dep_dir}/NeuralAmpModelerCore/Dependencies/eigen"
)
target_compile_options(neuralaudio PRIVATE
    -fPIC -DANDROID -O3 -std=c++17 -funroll-loops -DNDEBUG -DWAVENET_FRAMES=128
    -DRTNEURAL_DEFAULT_ALIGNMENT=32 -DRTNEURAL_USE_EIGEN=1 -DRTNEURAL_NAMESPACE=RTNeural
    -DDSP_SAMPLE_FLOAT -DNAM_SAMPLE_FLOAT -Dneural_amp_modeler_EXPORTS
    -Wno-sign-compare -Wno-reorder -Wno-infinite-recursion -Wno-unused-private-field
    -Wno-pessimizing-move -fvisibility=hidden)

# ─── Phase 2a: libfftconvolver.a (shared function) ──────────────────────────
add_fftconvolver_library(fftconvolver "${_nr_src}" "${_nr_plugin}" "${SNDFILE_PREFIX}/include" shared_libsndfile)

# ─── Phase 2a: libzita-resampler.a (shared function) ────────────────────────
add_zita_resampler_library(nr_zita_resampler "${_nr_plugin}")

# ─── Phase 2b: DSP plugin (Neuralrack.so) ────────────────────────────────────
set(_nr_lv2_compat "${_nr_build}/lv2_compat")
generate_lv2_compat_headers("${_nr_lv2_compat}")

add_library(neuralrack_dsp SHARED
    "${_nr_plugin}/lv2/NeuralRack.cpp"
    "${_nr_plugin}/engine/NeuralModelLoader.cpp"
)
target_include_directories(neuralrack_dsp PRIVATE
    "${_nr_lv2_compat}" "${LV2_INCLUDE}" "${_nr_plugin}" "${_nr_plugin}/engine" "${_nr_plugin}/lv2"
    "${_nr_plugin}/zita-resampler-1.1.0" "${_nr_src}/FFTConvolver"
    "${_nr_na_dir}" "${_nr_dep_dir}/RTNeural/" "${_nr_dep_dir}/math_approx/include/"
    "${_nr_dep_dir}" "${_nr_dep_dir}/NeuralAmpModelerCore/Dependencies/nlohmann/"
    "${_nr_dep_dir}/NeuralAmpModelerCore/Dependencies/eigen"
    "${SNDFILE_PREFIX}/include"
)
target_compile_options(neuralrack_dsp PRIVATE
    -fPIC -DANDROID -O3 -std=c++17 -funroll-loops -DNDEBUG -DUSE_ATOM -DWAVENET_FRAMES=128
    -DRTNEURAL_DEFAULT_ALIGNMENT=32 -DRTNEURAL_USE_EIGEN=1 -DRTNEURAL_NAMESPACE=RTNeural
    -DDSP_SAMPLE_FLOAT -DNAM_SAMPLE_FLOAT -Dneural_amp_modeler_EXPORTS
    -fvisibility=hidden -fdata-sections)
target_link_options(neuralrack_dsp PRIVATE
    -shared -Wl,--exclude-libs,ALL -Wl,--gc-sections -Wl,-z,noexecstack -Wl,--no-undefined)
target_link_libraries(neuralrack_dsp PRIVATE
    neuralaudio fftconvolver nr_zita_resampler
    "${SNDFILE_PREFIX}/lib/libsndfile.a"
    m log)
set_target_properties(neuralrack_dsp PROPERTIES
    OUTPUT_NAME "Neuralrack" SUFFIX ".so" PREFIX "" LIBRARY_OUTPUT_DIRECTORY "${_nr_build}")
add_dependencies(neuralrack_dsp shared_libsndfile lv2_libs)
lv2_strip_and_save_debug(neuralrack_dsp "${_nr_build}/Neuralrack.so")

# ─── Phase 2c: UI plugin (Neuralrack_ui.so) ──────────────────────────────────
set(_xputty_target "xputty_nr")
brummer_setup_xputty(${_xputty_target} "${_nr_xputty}" "${_nr_build}" "${_nr_plugin}/resources")

brummer_add_ui_target(neuralrack_ui "Neuralrack_ui" "${_nr_plugin}/gui/NeuralRack.c" "${_nr_build}" "${_xputty_target}" "${_nr_lv2_compat}"
    INCLUDES
        "${_nr_plugin}" "${_nr_plugin}/gui" "${_nr_plugin}/lv2"
        "${_nr_xputty}/header" "${_nr_xputty}/header/widgets" "${_nr_xputty}/header/dialogs"
        "${_nr_xputty}/resources" "${_nr_xputty}/lv2_plugin" "${_nr_xputty}/xdgmime"
    DEFINITIONS -DUSE_ATOM
)

# ─── Phase 3: Sync (stamp-based) ────────────────────────────────────────────
lv2_sync_dsp_ui(
    NAME neuralrack
    OUTPUT_NAME Neuralrack
    BUILD_DIR "${_nr_build}"
    ASSETS_DIR "${_nr_assets}"
    DSP_TARGET neuralrack_dsp
    UI_TARGET neuralrack_ui
)
