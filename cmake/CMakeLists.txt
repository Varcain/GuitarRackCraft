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
# cmake/CMakeLists.txt — Master build for all prebuild targets
#
# Cross-compiles LV2 libraries, X11/Cairo sysroot, FFTW3, 120+ GxPlugins,
# NAM, AIDA-X, NeuralRack, and generates plugin metadata.
#
# Usage:
#   cmake --preset android-arm64 -S cmake
#   cmake --build --preset android-arm64
# =============================================================================
cmake_minimum_required(VERSION 3.22)

# ─── Auto-detect Android NDK toolchain ────────────────────────────────────────
if(NOT CMAKE_TOOLCHAIN_FILE)
    # Try ANDROID_NDK env var
    if(DEFINED ENV{ANDROID_NDK} AND EXISTS "$ENV{ANDROID_NDK}/build/cmake/android.toolchain.cmake")
        set(CMAKE_TOOLCHAIN_FILE "$ENV{ANDROID_NDK}/build/cmake/android.toolchain.cmake"
            CACHE FILEPATH "Android NDK toolchain")
    else()
        # Auto-detect from ANDROID_HOME or ~/Android/Sdk
        set(_android_home "$ENV{ANDROID_HOME}")
        if(NOT _android_home)
            set(_android_home "$ENV{HOME}/Android/Sdk")
        endif()
        if(EXISTS "${_android_home}/ndk")
            file(GLOB _ndks "${_android_home}/ndk/*")
            if(_ndks)
                list(SORT _ndks COMPARE NATURAL ORDER DESCENDING)
                list(GET _ndks 0 _ndk)
                if(EXISTS "${_ndk}/build/cmake/android.toolchain.cmake")
                    set(CMAKE_TOOLCHAIN_FILE "${_ndk}/build/cmake/android.toolchain.cmake"
                        CACHE FILEPATH "Android NDK toolchain")
                endif()
            endif()
        endif()
    endif()
    if(NOT CMAKE_TOOLCHAIN_FILE)
        message(FATAL_ERROR
            "Cannot find Android NDK. Either:\n"
            "  - Set ANDROID_NDK env var to NDK root\n"
            "  - Set ANDROID_HOME env var (will find latest NDK)\n"
            "  - Pass -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake")
    endif()
endif()

if(NOT ANDROID_ABI)
    set(ANDROID_ABI "arm64-v8a" CACHE STRING "Android ABI")
endif()
if(NOT ANDROID_PLATFORM)
    set(ANDROID_PLATFORM "android-26" CACHE STRING "Android platform level")
endif()

project(guitarrackcraft-prebuild LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
include(ExternalProject)

# ─── Project paths ────────────────────────────────────────────────────────────
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Project root" FORCE)
set(THIRD_PARTY  "${PROJECT_ROOT}/3rd_party")
set(APP_ROOT     "${PROJECT_ROOT}/app/src/main" CACHE PATH "App root")
set(ASSETS_DIR   "${APP_ROOT}/assets/lv2" CACHE PATH "Assets dir")
set(JNILIBS_DIR  "${APP_ROOT}/jniLibs/arm64-v8a" CACHE PATH "jniLibs dir")

# ─── Include helper modules ──────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/modules")
include(AndroidNDK)
include(MesonCrossFile)
include(LV2Compat)
include(WatchSources)
include(LV2PluginUtils)
include(SharedLibs)
include(BrummerPlugin)
include(InitSubmodules)
setup_submodules()
include(ExternalBuild)

# ─── Generate build-time configurations ──────────────────────────────────────

# LV2 library prefix (where static libs + headers are installed)
set(LV2_PREFIX    "${APP_ROOT}/cpp/libs/lv2" CACHE PATH "LV2 prefix")
set(LV2_BUILD_DIR "${PROJECT_ROOT}/build/lv2" CACHE PATH "LV2 build dir")
set(LV2_INCLUDE   "${THIRD_PARTY}/lv2/include" CACHE PATH "LV2 include dir")
file(MAKE_DIRECTORY "${LV2_BUILD_DIR}" "${LV2_PREFIX}/lib" "${LV2_PREFIX}/include")

generate_meson_cross_file("${LV2_BUILD_DIR}/android_cross.txt" "${LV2_PREFIX}")

# X11/Cairo sysroot
set(X11_BUILD_DIR "${PROJECT_ROOT}/build/x11_ui" CACHE PATH "X11 build dir")
set(X11_SYSROOT   "${X11_BUILD_DIR}/sysroot" CACHE PATH "X11 sysroot")
file(MAKE_DIRECTORY "${X11_BUILD_DIR}" "${X11_SYSROOT}/lib" "${X11_SYSROOT}/include")

generate_meson_cross_file("${X11_BUILD_DIR}/android_cross.txt" "${X11_SYSROOT}")

# Mesa-specific cross-file (system='linux', -DMESA_FORCE_LINUX)
set(MESA_BUILD_DIR "${PROJECT_ROOT}/build/mesa" CACHE PATH "Mesa build dir")
file(MAKE_DIRECTORY "${MESA_BUILD_DIR}")
generate_mesa_cross_file("${MESA_BUILD_DIR}/mesa_cross.txt" "${X11_SYSROOT}")

# FFTW3
set(FFTW3_BUILD_DIR "${PROJECT_ROOT}/build/fftw3" CACHE PATH "FFTW3 build dir")
set(FFTW3_PREFIX    "${FFTW3_BUILD_DIR}/install" CACHE PATH "FFTW3 prefix")

# LV2 compat headers
set(LV2_COMPAT_DIR "${PROJECT_ROOT}/build/lv2_wrapper")
generate_lv2_compat_headers("${LV2_COMPAT_DIR}")
generate_faust_compat_header("${LV2_COMPAT_DIR}/gx_android_compat.h")
generate_sigcpp_shim("${LV2_COMPAT_DIR}")
generate_sndfile_stub("${LV2_COMPAT_DIR}/sndfile.hh")

# ─── Shared libsndfile (used by neuralrack, impulseloader) ───────────────────
set(SNDFILE_BUILD_DIR "${PROJECT_ROOT}/build/shared/libsndfile" CACHE PATH "Shared libsndfile build dir")
set(SNDFILE_PREFIX    "${PROJECT_ROOT}/build/shared/sndfile_prefix" CACHE PATH "Shared libsndfile install prefix")
file(MAKE_DIRECTORY "${SNDFILE_BUILD_DIR}" "${SNDFILE_PREFIX}")
add_shared_libsndfile(shared_libsndfile "${SNDFILE_BUILD_DIR}" "${SNDFILE_PREFIX}")

# ─── Global stubs ────────────────────────────────────────────────────────────
add_library(xshm_stub STATIC "${APP_ROOT}/cpp/xshm_stub.c")
target_compile_options(xshm_stub PRIVATE -fPIC -DANDROID -O2)

# ─── Phase 1: Sysroot dependencies ───────────────────────────────────────────
# LV2 libs: serd → zix → sord → sratom → lilv
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/lv2_libs.cmake)

# FFTW3 (single-precision, static)
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/fftw3.cmake)

# X11/Cairo/Mesa sysroot
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/x11_sysroot.cmake)

# ─── Phase 2: Plugin compilation (native CMake targets) ──────────────────────
# GxPlugins DSP .so
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/gx_plugins.cmake)

# Guitarix trunk plugins DSP .so
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/trunk_plugins.cmake)

# Plugin UI .so (xputty + X11/Cairo)
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/plugin_uis.cmake)

# ─── Phase 3: CMake-wrapped plugin builds ────────────────────────────────────
# Neural Amp Modeler
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/nam.cmake)

# AIDA-X (headless, DSP-only)
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/aidax.cmake)

# AIDA-X (full, with EGL+GLES2 UI)
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/aidax_full.cmake)

# NeuralRack (DSP + UI)
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/neuralrack.cmake)

# ImpulseLoader (DSP + UI)
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/impulseloader.cmake)

# XDarkTerror (DSP + UI)
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/xdarkterror.cmake)

# XTinyTerror (DSP + UI)
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/xtinyterror.cmake)

# CollisionDrive (DSP + UI)
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/collisiondrive.cmake)

# MetalTone (DSP + UI)
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/metaltone.cmake)

# GxCabSim (DSP + UI)
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/gxcabsim.cmake)

# ModularAmpToolKit (4 plugins: PreAmps, PowerAmps, PreAmpImpulses, PowerAmpImpulses)
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/modamptk.cmake)

# FatFrog (DSP + UI)
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/fatfrog.cmake)

# DoubleTracker (DSP-only, Faust)
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/doubletracker.cmake)

# ─── Phase 4: Metadata + modgui ──────────────────────────────────────────────
include(${CMAKE_CURRENT_SOURCE_DIR}/targets/metadata.cmake)

# ─── Top-level targets ───────────────────────────────────────────────────────

# Build everything
add_custom_target(all_plugins
    DEPENDS
        gx_plugins_done
        trunk_plugins_done
        plugin_uis_done
        nam_done
        aidax_done
        aidax_full_done
        neuralrack_done
        impulseloader_done
        xdarkterror_done
        xtinyterror_done
        collisiondrive_done
        metaltone_done
        gxcabsim_done
        modamptk_done
        fatfrog_done
        doubletracker_done
        metadata_done
    COMMENT "All plugins built"
)
