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

# Minimal DSP-only CMakeLists.txt for AIDA-X LV2 plugin.
# Used for native TTL generation (avoids OpenGL/X11 UI dependencies).
# Usage: cmake -S <this-dir> -B <build-dir> -DAIDAX_SRC=<path-to-AIDA-X>
cmake_minimum_required(VERSION 3.15)
project(AIDA-X VERSION 1.1.0)

set(CMAKE_POLICY_DEFAULT_CMP0063 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN TRUE)

# AIDA-X's DistrhoPluginCommon.hpp unconditionally sets DISTRHO_UI_FILE_BROWSER=1.
# DPF's DistrhoPluginChecks.h then requires DGL_USE_FILE_BROWSER to be defined.
# In the real CMake build, dpf_add_plugin(... USE_FILE_BROWSER) handles this.
# For our DSP-only overlay, just define it to pass the compile-time check.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDISTRHO_NAMESPACE=AidaDISTRHO -DDGL_NAMESPACE=AidaDGL -DDGL_USE_FILE_BROWSER")

set(AIDAX_SRC "" CACHE PATH "Path to AIDA-X source directory")
if(NOT AIDAX_SRC OR NOT EXISTS "${AIDAX_SRC}/CMakeLists.txt")
    message(FATAL_ERROR "AIDAX_SRC must point to the AIDA-X source directory")
endif()

set(RTNEURAL_XSIMD ON CACHE BOOL "Use xsimd backend for RTNeural")

add_subdirectory("${AIDAX_SRC}/modules/dpf" dpf)
add_subdirectory("${AIDAX_SRC}/modules/rtneural" rtneural)

find_package(Threads REQUIRED)
set_property(GLOBAL PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

dpf_add_plugin(AIDA-X
    TARGETS lv2
    FILES_DSP
        Files.cpp
        "${AIDAX_SRC}/modules/FFTConvolver/AudioFFT.cpp"
        "${AIDAX_SRC}/modules/FFTConvolver/FFTConvolver.cpp"
        "${AIDAX_SRC}/modules/FFTConvolver/TwoStageFFTConvolver.cpp"
        "${AIDAX_SRC}/modules/FFTConvolver/Utilities.cpp"
        "${AIDAX_SRC}/modules/r8brain/pffft.cpp"
        "${AIDAX_SRC}/modules/r8brain/r8bbase.cpp"
        "${AIDAX_SRC}/src/aidadsp-plugin.cpp"
        "${AIDAX_SRC}/src/Biquad.cpp"
        "${AIDAX_SRC}/src/3rd-party.cpp")

target_include_directories(AIDA-X PUBLIC
    "${AIDAX_SRC}/src"
    "${AIDAX_SRC}/src/plugin"
    "${AIDAX_SRC}/modules/dr_libs"
    "${AIDAX_SRC}/modules/FFTConvolver"
    "${AIDAX_SRC}/modules/r8brain"
    "${AIDAX_SRC}/modules/rtneural"
    "${CMAKE_BINARY_DIR}")

target_link_libraries(AIDA-X PUBLIC RTNeural ${CMAKE_THREAD_LIBS_INIT})

# Generate Files.cpp from embedded resources
add_custom_command(
    PRE_BUILD
    COMMAND "${AIDAX_SRC}/modules/dpf/utils/res2c.py" Files "${AIDAX_SRC}/files" "${CMAKE_BINARY_DIR}"
    MAIN_DEPENDENCY "${AIDAX_SRC}/files"
    OUTPUT Files.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
