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
# cmake/modules/SharedLibs.cmake — Shared library definitions for multiple plugins
#
# Provides reusable functions for libsndfile, FFTConvolver, and zita-resampler
# that are needed by neuralrack, impulseloader, and modamptk.
# =============================================================================

# ─── libsndfile (ExternalProject, built once, shared across plugins) ────────
# Usage: add_shared_libsndfile(TARGET BUILD_DIR INSTALL_PREFIX)
function(add_shared_libsndfile TARGET BUILD_DIR INSTALL_PREFIX)
    set(_sndfile_src "${THIRD_PARTY}/libsndfile")
    ExternalProject_Add(${TARGET}
        SOURCE_DIR      "${_sndfile_src}"
        BINARY_DIR      "${BUILD_DIR}"
        INSTALL_DIR     "${INSTALL_PREFIX}"
        CMAKE_ARGS
            -DCMAKE_POLICY_DEFAULT_CMP0057=NEW
            -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
            -DANDROID_ABI=${ANDROID_ABI}
            -DANDROID_PLATFORM=${ANDROID_PLATFORM}
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
            -DBUILD_SHARED_LIBS=OFF
            -DBUILD_PROGRAMS=OFF
            -DBUILD_EXAMPLES=OFF
            -DBUILD_TESTING=OFF
            -DENABLE_EXTERNAL_LIBS=OFF
            -DENABLE_MPEG=OFF
            -DINSTALL_MANPAGES=OFF
        BUILD_COMMAND   ${CMAKE_COMMAND} --build <BINARY_DIR> -j${NJOBS}
        INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
        BUILD_BYPRODUCTS "${INSTALL_PREFIX}/lib/libsndfile.a"
    )
    watch_external_sources(${TARGET} DIRECTORIES "${_sndfile_src}/src")
endfunction()

# ─── FFTConvolver static library ─────────────────────────────────────────────
# Usage: add_fftconvolver_library(TARGET SOURCE_ROOT PLUGIN_DIR SNDFILE_INCLUDE SNDFILE_TARGET)
function(add_fftconvolver_library TARGET SOURCE_ROOT PLUGIN_DIR SNDFILE_INCLUDE SNDFILE_TARGET)
    add_library(${TARGET} STATIC
        "${SOURCE_ROOT}/FFTConvolver/AudioFFT.cpp"
        "${SOURCE_ROOT}/FFTConvolver/FFTConvolver.cpp"
        "${SOURCE_ROOT}/FFTConvolver/TwoStageFFTConvolver.cpp"
        "${SOURCE_ROOT}/FFTConvolver/Utilities.cpp"
        "${PLUGIN_DIR}/engine/fftconvolver.cpp"
    )
    target_include_directories(${TARGET} PRIVATE
        "${SOURCE_ROOT}/FFTConvolver" "${PLUGIN_DIR}" "${PLUGIN_DIR}/engine"
        "${PLUGIN_DIR}/zita-resampler-1.1.0" "${SNDFILE_INCLUDE}"
    )
    target_compile_options(${TARGET} PRIVATE
        -fPIC -DANDROID -O3 -std=c++17 -funroll-loops -DNDEBUG -DUSE_ATOM -fvisibility=hidden
        -Wno-sign-compare -Wno-reorder)
    add_dependencies(${TARGET} ${SNDFILE_TARGET})
endfunction()

# ─── zita-resampler static library ───────────────────────────────────────────
# Usage: add_zita_resampler_library(TARGET PLUGIN_DIR)
function(add_zita_resampler_library TARGET PLUGIN_DIR)
    add_library(${TARGET} STATIC
        "${PLUGIN_DIR}/zita-resampler-1.1.0/resampler.cc"
        "${PLUGIN_DIR}/zita-resampler-1.1.0/resampler-table.cc"
        "${PLUGIN_DIR}/engine/gx_resampler.cc"
    )
    target_include_directories(${TARGET} PRIVATE
        "${PLUGIN_DIR}/zita-resampler-1.1.0" "${PLUGIN_DIR}/engine"
    )
    target_compile_options(${TARGET} PRIVATE
        -fPIC -DANDROID -O3 -std=c++17 -DNDEBUG -fvisibility=hidden)
endfunction()
